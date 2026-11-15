/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "MilitaryTrash40.h"

#include "Ai/Raid/RaidClear/Common/Interrupts.h"
#include "Ai/Raid/RaidClear/Common/KillOrder.h"
#include "Ai/Raid/RaidClear/Common/WayOut.h"
#include "CombatManager.h"
#include "Creature.h"
#include "Group.h"
#include "Naxx40Encounter.h"
#include "ReachTargetActions.h"
#include "RtiTargetValue.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace
{
    constexpr uint32 UnholyAxe = 351061;
    constexpr uint32 AxeWhirlwindAura = 24236;
    constexpr uint32 AxeWhirlwindDamage = 15589;
    constexpr uint32 CaptainWhirlwind = 28335;
    constexpr uint32 ShadowBoltVolley = 28448;
    constexpr float FloorBand = 5.0f;
}

bool IsNaxx40MilitaryTrash(Unit const& unit)
{
    // Includes unmodified Death Touched Warrior and Death Lord spawns in the lower corridor.
    constexpr std::array entries = {Naxx40DeathknightCaptain, 351049u, 351050u, 351051u, 351052u, 351053u, 351054u,
        351055u, Naxx40ShadeOfNaxxramas, 351057u, 351058u, 351060u, UnholyAxe, 351062u, 351063u, 16157u, 16158u, 16861u};
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

bool CanEngageNaxx40NecropolisAcolyte(Player const& bot, Unit const& target)
{
    Creature const* acolyte = target.ToCreature();
    return acolyte && acolyte->GetEntry() == Naxx40NecropolisAcolyte && acolyte->IsAlive() &&
           acolyte->IsInCombat() && !acolyte->IsEvadingAttacks() &&
           std::abs(bot.GetPositionZ() - acolyte->GetPositionZ()) <= FloorBand && bot.IsWithinLOSInMap(acolyte);
}

namespace
{
    constexpr char NearWhirlwind[] = "naxx40 military whirlwind too close";
    constexpr char StayClear[] = "naxx40 military stay clear";
    constexpr char AcolyteCasting[] = "naxx40 acolyte casting volley";
    constexpr char InterruptAcolyte[] = "naxx40 interrupt acolyte volley";
    constexpr float WhirlwindSearch = 40.0f;
    constexpr float MovementMargin = 1.0f;

    char const* VolleyInterrupt(PlayerbotAI* ai, Player* bot, Unit* target)
    {
        if (!target || target->GetEntry() != Naxx40NecropolisAcolyte ||
            !CanEngageNaxx40NecropolisAcolyte(*bot, *target) || !bot->IsValidAttackTarget(target))
            return nullptr;

        Spell const* spell = target->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        SpellInfo const* info = spell ? spell->GetSpellInfo() : nullptr;
        if (!info || info->Id != ShadowBoltVolley || RaidClear::InterruptJustSent(target))
            return nullptr;

        return RaidClear::ReadyStop(ai, bot, target, info, false);
    }

    Unit* AcolyteToInterrupt(PlayerbotAI* ai, Player* bot, char const*& interrupt)
    {
        Group* group = bot->GetGroup();
        if (!group || !bot->IsInCombat() || !IsNaxx40Map(bot->FindMap()))
            return nullptr;

        Unit* current = ai->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
        interrupt = VolleyInterrupt(ai, bot, current);
        if (interrupt)
            return current;

        Unit* skull = ai->GetUnit(group->GetTargetIcon(RtiTargetValue::skullIndex));
        if (skull == current)
            return nullptr;
        interrupt = VolleyInterrupt(ai, bot, skull);
        return interrupt ? skull : nullptr;
    }

    class AcolyteVolleyTrigger final : public Trigger
    {
    public:
        explicit AcolyteVolleyTrigger(PlayerbotAI* ai) : Trigger(ai, AcolyteCasting) {}

        bool IsActive() override
        {
            char const* interrupt = nullptr;
            return AcolyteToInterrupt(botAI, bot, interrupt) != nullptr;
        }
    };

    class InterruptAcolyteVolleyAction final : public Action
    {
    public:
        explicit InterruptAcolyteVolleyAction(PlayerbotAI* ai) : Action(ai, InterruptAcolyte) {}

        bool Execute(Event /*event*/) override
        {
            char const* interrupt = nullptr;
            Unit* acolyte = AcolyteToInterrupt(botAI, bot, interrupt);
            if (!acolyte || !botAI->CastSpell(interrupt, acolyte))
                return false;

            RaidClear::InterruptSent(acolyte);
            return true;
        }
    };

    struct WhirlwindSource
    {
        Unit const* unit;
        float radius;
        bool active;
    };

    float WhirlwindRadius(uint32 entry)
    {
        auto const readRadius = [](uint32 spellId)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            float const yards = info ? info->Effects[EFFECT_0].CalcRadius() : 0.0f;
            return yards > 0.0f ? yards : 8.0f;
        };
        static float const captainRadius = readRadius(CaptainWhirlwind);
        static float const axeRadius = readRadius(AxeWhirlwindDamage);
        return entry == UnholyAxe ? axeRadius : captainRadius;
    }

    bool NeedsRangedSpace(Player* bot)
    {
        return PlayerbotAI::IsHeal(bot) || !PlayerbotAI::IsMelee(bot);
    }

    std::vector<WhirlwindSource> WhirlwindSources(PlayerbotAI* ai, Player* bot)
    {
        std::vector<WhirlwindSource> sources;
        if (!IsNaxx40Map(bot->FindMap()) || !bot->GetGroup() || PlayerbotAI::IsTank(bot))
            return sources;

        for (ObjectGuid const& guid : ai->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get())
        {
            Unit const* enemy = ai->GetUnit(guid);
            if (!enemy || (enemy->GetEntry() != Naxx40DeathknightCaptain && enemy->GetEntry() != UnholyAxe) ||
                !enemy->IsAlive() || !enemy->IsInCombat() || !bot->IsValidAttackTarget(enemy) ||
                bot->GetExactDist(enemy) > WhirlwindSearch)
                continue;
            bool const active = enemy->GetEntry() == UnholyAxe && enemy->HasAura(AxeWhirlwindAura);
            sources.push_back({enemy, WhirlwindRadius(enemy->GetEntry()), active});
        }
        return sources;
    }

    // Ranged and healers keep clear from the start. Melee avoid overlapping
    // attacks and step away from a single Axe during its ticking aura.
    float WhirlwindGap(std::vector<WhirlwindSource> const& sources, bool ranged, float x, float y, float z)
    {
        float nearest = std::numeric_limits<float>::max();
        float second = nearest;
        float active = nearest;
        for (auto const& source : sources)
        {
            if (std::abs(source.unit->GetPositionZ() - z) > FloorBand)
                continue;
            float const gap = source.unit->GetExactDist2d(x, y) - source.radius;
            second = std::min(second, std::max(nearest, gap));
            nearest = std::min(nearest, gap);
            if (source.active)
                active = std::min(active, gap);
        }
        return ranged ? nearest : std::min(active, second);
    }

    float WhirlwindGap(std::vector<WhirlwindSource> const& sources, bool ranged, Unit const& unit)
    {
        return WhirlwindGap(sources, ranged, unit.GetPositionX(), unit.GetPositionY(), unit.GetPositionZ());
    }

    float MinimumGap(bool ranged)
    {
        return ranged ? 2.0f : MovementMargin;
    }

    class WhirlwindProximityTrigger final : public Trigger
    {
    public:
        explicit WhirlwindProximityTrigger(PlayerbotAI* ai) : Trigger(ai, NearWhirlwind) {}

        bool IsActive() override
        {
            if (!bot->IsAlive() || !botAI->CanMove())
                return false;
            bool const ranged = NeedsRangedSpace(bot);
            return WhirlwindGap(WhirlwindSources(botAI, bot), ranged, *bot) < MinimumGap(ranged);
        }
    };

    class StayClearOfWhirlwindAction final : public Action
    {
    public:
        explicit StayClearOfWhirlwindAction(PlayerbotAI* ai) : Action(ai, StayClear), _walk(ai) {}

        bool Execute(Event /*event*/) override
        {
            auto const sources = WhirlwindSources(botAI, bot);
            bool const ranged = NeedsRangedSpace(bot);
            float const keepAway = MinimumGap(ranged);
            if (WhirlwindGap(sources, ranged, *bot) >= keepAway)
                return false;

            auto const gapAt = [&sources, ranged](float x, float y, float z)
            {
                return WhirlwindGap(sources, ranged, x, y, z);
            };
            return _walk.Go(gapAt, keepAway, ranged ? 4.0f : 2.0f, MovementMargin, 15.0f, sources.size() == 1);
        }

    private:
        RaidClear::WayOutWalk _walk;
    };

    class WhirlwindSpacingMultiplier final : public Multiplier
    {
    public:
        explicit WhirlwindSpacingMultiplier(PlayerbotAI* ai) : Multiplier(ai, "naxx40 military whirlwind spacing") {}

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

            auto const sources = WhirlwindSources(botAI, bot);
            if (sources.empty())
                return 1.0f;

            bool const ranged = NeedsRangedSpace(bot);
            Unit const* target = follow ? RaidClear::KillOrderMarker(bot->GetGroup()) : action->GetTarget();
            return target && WhirlwindGap(sources, ranged, *target) < MinimumGap(ranged) ? 0.0f : 1.0f;
        }
    };

    class MilitaryTrashStrategy final : public Strategy
    {
    public:
        explicit MilitaryTrashStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string const getName() override { return Naxx40MilitaryTrashStrategyName; }

        void InitTriggers(std::vector<TriggerNode*>& triggers) override
        {
            triggers.push_back(new TriggerNode(NearWhirlwind, {NextAction(StayClear, ACTION_EMERGENCY + 3)}));
            triggers.push_back(new TriggerNode("rc kill order", {NextAction("rc mark kill order", ACTION_RAID + 2)}));
            triggers.push_back(new TriggerNode(AcolyteCasting, {NextAction(InterruptAcolyte, ACTION_INTERRUPT + 1)}));
        }

        void InitMultipliers(std::vector<Multiplier*>& multipliers) override
        {
            multipliers.push_back(new WhirlwindSpacingMultiplier(botAI));
        }
    };
}

Naxx40MilitaryActionContext::Naxx40MilitaryActionContext()
{
    creators[StayClear] = [](PlayerbotAI* ai) { return new StayClearOfWhirlwindAction(ai); };
    creators[InterruptAcolyte] = [](PlayerbotAI* ai) { return new InterruptAcolyteVolleyAction(ai); };
}

Naxx40MilitaryTriggerContext::Naxx40MilitaryTriggerContext()
{
    creators[NearWhirlwind] = [](PlayerbotAI* ai) { return new WhirlwindProximityTrigger(ai); };
    creators[AcolyteCasting] = [](PlayerbotAI* ai) { return new AcolyteVolleyTrigger(ai); };
}

Naxx40MilitaryStrategyContext::Naxx40MilitaryStrategyContext()
{
    creators[Naxx40MilitaryTrashStrategyName] = [](PlayerbotAI* ai) { return new MilitaryTrashStrategy(ai); };
}
