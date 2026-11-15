/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MilitaryTrash40.h"

#include "Ai/Raid/RaidClear/Common/KillOrder.h"
#include "Ai/Raid/RaidClear/Common/WayOut.h"
#include "CombatManager.h"
#include "Naxx40Encounter.h"
#include "ReachTargetActions.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

bool IsNaxx40MilitaryTrash(Unit const& unit)
{
    // Includes unmodified Death Touched Warrior and Death Lord spawns in the lower corridor.
    constexpr std::array entries = {Naxx40DeathknightCaptain, 351049u, 351050u, 351051u, 351052u, 351053u, 351054u,
        351055u, Naxx40ShadeOfNaxxramas, 351057u, 351058u, 351060u, 351061u, 351062u, 351063u, 16157u, 16158u, 16861u};
    return std::ranges::find(entries, unit.GetEntry()) != entries.end() && IsNaxx40Map(unit.FindMap());
}

bool MayPetAttackNaxx40MilitaryTrash(Player const& owner, Unit const& target)
{
    if (!owner.GetGroup() || !IsNaxx40MilitaryTrash(target))
        return true;

    // A mob already in combat with a pet alone still belongs to an unclaimed pack.
    for (auto const& [guid, reference] : target.GetCombatManager().GetPvECombatRefs())
    {
        if (!guid.IsPlayer() || reference->IsSuppressedFor(&target))
            continue;
        Player const* member = reference->GetOther(&target)->ToPlayer();
        if (member && member->IsAlive() && member->GetGroup() == owner.GetGroup())
            return true;
    }
    return false;
}

namespace
{
    constexpr char NearCaptain[] = "naxx40 military captain too close";
    constexpr char StayClear[] = "naxx40 military stay clear";
    constexpr float CaptainSearch = 40.0f;
    constexpr float FloorBand = 5.0f;
    constexpr float MovementMargin = 1.0f;

    float WhirlwindRadius()
    {
        // The spell deals instant weapon damage and stuns the target, so start clear before the cast.
        static float const radius = []
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(28335);
            float const yards = info ? info->Effects[EFFECT_0].CalcRadius() : 0.0f;
            return yards > 0.0f ? yards : 8.0f;
        }();
        return radius;
    }

    bool NeedsRangedSpace(Player* bot)
    {
        return PlayerbotAI::IsHeal(bot) || !PlayerbotAI::IsMelee(bot);
    }

    std::vector<Unit const*> Captains(PlayerbotAI* ai, Player* bot)
    {
        std::vector<Unit const*> captains;
        if (!IsNaxx40Map(bot->FindMap()) || !bot->GetGroup() || PlayerbotAI::IsTank(bot))
            return captains;

        for (ObjectGuid const& guid : ai->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
        {
            Unit const* captain = ai->GetUnit(guid);
            if (!captain || captain->GetEntry() != Naxx40DeathknightCaptain || !captain->IsAlive() ||
                !captain->IsInCombat() || !bot->IsValidAttackTarget(captain) ||
                bot->GetExactDist(captain) > CaptainSearch)
                continue;
            captains.push_back(captain);
        }
        return captains;
    }

    // A melee DPS avoids overlap between captains. Healers and ranged keep clear of each one.
    float CaptainDistance(std::vector<Unit const*> const& captains, bool ranged, float x, float y, float z)
    {
        float nearest = std::numeric_limits<float>::max();
        float second = nearest;
        for (Unit const* captain : captains)
        {
            if (std::abs(captain->GetPositionZ() - z) > FloorBand)
                continue;
            float const distance = captain->GetExactDist2d(x, y);
            second = std::min(second, std::max(nearest, distance));
            nearest = std::min(nearest, distance);
        }
        return ranged ? nearest : second;
    }

    float CaptainDistance(std::vector<Unit const*> const& captains, bool ranged, Unit const& unit)
    {
        return CaptainDistance(captains, ranged, unit.GetPositionX(), unit.GetPositionY(), unit.GetPositionZ());
    }

    float MinimumDistance(bool ranged)
    {
        return WhirlwindRadius() + (ranged ? 2.0f : MovementMargin);
    }

    class CaptainProximityTrigger final : public Trigger
    {
    public:
        explicit CaptainProximityTrigger(PlayerbotAI* ai) : Trigger(ai, NearCaptain) {}

        bool IsActive() override
        {
            if (!bot->IsAlive() || !botAI->CanMove())
                return false;
            bool const ranged = NeedsRangedSpace(bot);
            return CaptainDistance(Captains(botAI, bot), ranged, *bot) < MinimumDistance(ranged);
        }
    };

    class StayClearOfCaptainsAction final : public Action
    {
    public:
        explicit StayClearOfCaptainsAction(PlayerbotAI* ai) : Action(ai, StayClear), _walk(ai) {}

        bool Execute(Event /*event*/) override
        {
            auto const captains = Captains(botAI, bot);
            bool const ranged = NeedsRangedSpace(bot);
            float const keepAway = MinimumDistance(ranged);
            if (CaptainDistance(captains, ranged, *bot) >= keepAway)
                return false;

            auto const distanceAt = [&captains, ranged](float x, float y, float z)
            {
                return CaptainDistance(captains, ranged, x, y, z);
            };
            return _walk.Go(distanceAt, keepAway, WhirlwindRadius() + (ranged ? 4.0f : 2.0f),
                            MovementMargin, 15.0f, ranged && captains.size() == 1);
        }

    private:
        RaidClear::WayOutWalk _walk;
    };

    class CaptainSpacingMultiplier final : public Multiplier
    {
    public:
        explicit CaptainSpacingMultiplier(PlayerbotAI* ai) : Multiplier(ai, "naxx40 military captain spacing") {}

        float GetValue(Action* action) override
        {
            if (!action || !IsNaxx40Map(bot->FindMap()) || PlayerbotAI::IsTank(bot))
                return 1.0f;

            // Keep safe ranged casts and healing available. Suppress unsafe melee approaches,
            // and ordinary follow if it would put the bot directly on the tank inside the circle.
            std::string const& name = action->getName();
            bool const follow = name == "follow" || name == "dungeon clear follow tank";
            bool const melee = dynamic_cast<ReachMeleeAction*>(action) || name == "melee";
            if (!follow && !melee)
                return 1.0f;

            auto const captains = Captains(botAI, bot);
            bool const ranged = NeedsRangedSpace(bot);
            if (captains.empty() || (!ranged && captains.size() < 2))
                return 1.0f;

            Unit const* target = follow ? RaidClear::KillOrderMarker(bot->GetGroup()) : action->GetTarget();
            return target && CaptainDistance(captains, ranged, *target) < MinimumDistance(ranged) ? 0.0f : 1.0f;
        }
    };

    class MilitaryTrashStrategy final : public Strategy
    {
    public:
        explicit MilitaryTrashStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string const getName() override { return Naxx40MilitaryTrashStrategyName; }

        void InitTriggers(std::vector<TriggerNode*>& triggers) override
        {
            triggers.push_back(new TriggerNode(NearCaptain, {NextAction(StayClear, ACTION_EMERGENCY + 3)}));
            triggers.push_back(new TriggerNode("rc kill order", {NextAction("rc mark kill order", ACTION_RAID + 2)}));
        }

        void InitMultipliers(std::vector<Multiplier*>& multipliers) override
        {
            multipliers.push_back(new CaptainSpacingMultiplier(botAI));
        }
    };
}

Naxx40MilitaryActionContext::Naxx40MilitaryActionContext()
{
    creators[StayClear] = [](PlayerbotAI* ai) { return new StayClearOfCaptainsAction(ai); };
}

Naxx40MilitaryTriggerContext::Naxx40MilitaryTriggerContext()
{
    creators[NearCaptain] = [](PlayerbotAI* ai) { return new CaptainProximityTrigger(ai); };
}

Naxx40MilitaryStrategyContext::Naxx40MilitaryStrategyContext()
{
    creators[Naxx40MilitaryTrashStrategyName] = [](PlayerbotAI* ai) { return new MilitaryTrashStrategy(ai); };
}
