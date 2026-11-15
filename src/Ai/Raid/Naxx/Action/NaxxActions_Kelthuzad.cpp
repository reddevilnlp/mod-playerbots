/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NaxxActions.h"
#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "PathGenerator.h"
#include "Pet.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "SpellAuras.h"
#include "SpellMgr.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <list>
#include <tuple>
#include <vector>

namespace
{
constexpr float SoldierInterceptRadius = 20.0f;
constexpr uint32 SoulWeaver40Entry = 351075;
constexpr float WeaverCleanupSearchRadius = 65.0f;
constexpr uint32 FrostBlastAura = 27808;
constexpr float FrostBlastHealRange = 35.0f;
constexpr float FrostBlastHealHealth = 65.0f;

bool HealFrostBlast(PlayerbotAI& ai, Unit const& boss)
{
    Player* bot = ai.GetBot();
    Group* group = bot->GetGroup();
    if (!group || !PlayerbotAI::IsHeal(bot) || bot->IsNonMeleeSpellCast(true))
        return false;

    Player* target = nullptr;
    float lowest = FrostBlastHealHealth;
    for (GroupReference const* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsAlive() || !member->HasAura(FrostBlastAura, boss.GetGUID()) ||
            member->GetHealthPct() >= FrostBlastHealHealth ||
            !bot->IsWithinDistInMap(member, FrostBlastHealRange) || !bot->IsWithinLOSInMap(member))
            continue;

        float const urgency = member->GetHealthPct() - (boss.GetVictim() == member ? 15.0f : 0.0f);
        if (urgency < lowest)
        {
            target = member;
            lowest = urgency;
        }
    }
    if (!target)
        return false;

    std::array<char const*, 2> spells;
    switch (bot->getClass())
    {
        case CLASS_DRUID:   spells = {"swiftmend", "regrowth"}; break;
        case CLASS_PALADIN: spells = {"holy shock", "flash of light"}; break;
        case CLASS_PRIEST:   spells = {"power word: shield", "flash heal"}; break;
        case CLASS_SHAMAN:   spells = {"riptide", "lesser healing wave"}; break;
        default: return false;
    }
    for (char const* spell : spells)
        if (ai.CanCastSpell(spell, target) && ai.CastSpell(spell, target))
            return true;
    return false;
}

int IncomingPriority(Unit const& add, float distance)
{
    if (KelthuzadBossHelper::IsNaxx40Soldier(&add))
        return distance <= SoldierInterceptRadius ? 0 : 2;
    if (KelthuzadBossHelper::IsNaxx40RangedAdd(&add))
        return 1;
    Unit* victim = add.GetVictim();
    Player* player = victim ? victim->ToPlayer() : nullptr;
    return player && PlayerbotAI::IsTank(player) ? 4 : 3;
}

Unit* SelectIncoming(PlayerbotAI& ai, KelthuzadBossHelper const& helper, GuidVector const& attackers)
{
    Unit* incoming = nullptr;
    int priority = 0;
    float distance = 0.0f;
    for (ObjectGuid const& guid : attackers)
    {
        Unit* candidate = ai.GetUnit(guid);
        if (!helper.IsNaxx40Incoming(candidate))
            continue;

        // A Soldier about to reach the raid still comes first. Otherwise focus the slow
        // Weaver before it gets a melee swing and cones the camp.
        float const candidateDistance = candidate->GetExactDist2d(helper.center.first, helper.center.second);
        int const candidatePriority = IncomingPriority(*candidate, candidateDistance);
        if (!incoming || candidatePriority < priority || (candidatePriority == priority && candidateDistance < distance))
        {
            incoming = candidate;
            priority = candidatePriority;
            distance = candidateDistance;
        }
    }
    return incoming;
}

float WeaverDamageRange(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    char const* spell;
    switch (bot->getClass())
    {
        case CLASS_DRUID: spell = "wrath"; break;
        case CLASS_HUNTER: spell = "auto shot"; break;
        case CLASS_MAGE: spell = "frostbolt"; break;
        case CLASS_PRIEST: spell = "mind blast"; break;
        case CLASS_SHAMAN: spell = "lightning bolt"; break;
        case CLASS_WARLOCK: spell = "shadow bolt"; break;
        default: return ai.GetRange("spell");
    }

    uint32 const id = ai.GetAiObjectContext()->GetValue<uint32>("spell id", spell)->Get();
    SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
    return info ? info->GetMaxRange(false, bot) : ai.GetRange("spell");
}

Creature* SelectEngagedWeaver(PlayerbotAI& ai, Unit const& boss)
{
    Player const* bot = ai.GetBot();
    std::list<Creature*> weavers;
    boss.GetCreatureListWithEntryInGrid(weavers, SoulWeaver40Entry, WeaverCleanupSearchRadius);
    float const range = WeaverDamageRange(ai);
    Creature* selected = nullptr;
    std::tuple<bool, float, ObjectGuid> selectedPriority;
    for (Creature* weaver : weavers)
    {
        Unit const* victim = weaver->GetVictim();
        if (weaver->GetSummonerGUID() != boss.GetGUID() || !weaver->IsAlive() || !weaver->IsInCombat() ||
            !victim || !victim->IsAlive() || weaver->IsPolymorphed() ||
            weaver->HasAuraWithMechanic(1ULL << MECHANIC_BANISH) || !bot->IsValidAttackTarget(weaver) ||
            !bot->IsWithinCombatRange(weaver, range) || !bot->IsWithinLOSInMap(weaver))
            continue;

        // Focus a Weaver that has reached its target first, then finish the most wounded one.
        auto const priority = std::tuple(!weaver->IsWithinMeleeRange(victim), weaver->GetHealthPct(), weaver->GetGUID());
        if (!selected || priority < selectedPriority)
        {
            selected = weaver;
            selectedPriority = priority;
        }
    }
    return selected;
}

bool SelectWithoutMelee(Player& bot, AiObjectContext& context, Unit* target)
{
    bot.AttackStop();
    bot.SetSelection(target->GetGUID());
    context.GetValue<Unit*>("current target")->Set(target);
    return true;
}
}

namespace
{
    constexpr uint32 ShadowFissureEntry = 16129;
    constexpr uint32 DetonateMana = 27819;
    constexpr float FissureDanger = 5.5f;
    constexpr float FissureSafe = 7.0f;
    constexpr float DetonationSafe = 12.5f;
    constexpr float ArenaRadius = 44.0f;
    constexpr float QuarterTurn = static_cast<float>(M_PI) / 4.0f;

    Movement::PointsArray CheckedEscape(Player& bot, Position const& end, Position const& center,
                                        float lengthLimit, Creature const* fissure)
    {
        if (end.GetExactDist2d(&center) > ArenaRadius)
            return {};
        PathGenerator path(&bot);
        path.SetUseStraightPath(true);
        constexpr uint32 rejected = PATHFIND_INCOMPLETE | PATHFIND_NOPATH | PATHFIND_SHORTCUT |
                                    PATHFIND_NOT_USING_PATH | PATHFIND_FARFROMPOLY;
        if (!path.CalculatePath(end.GetPositionX(), end.GetPositionY(), end.GetPositionZ()) ||
            !(path.GetPathType() & PATHFIND_NORMAL) || (path.GetPathType() & rejected) ||
            path.getPathLength() > lengthLimit)
            return {};
        auto const& actual = path.GetActualEndPosition();
        if (end.GetExactDistSq(actual.x, actual.y, actual.z) > 0.75f * 0.75f)
            return {};

        auto const& points = path.GetPath();
        float const outerLimit = std::max(ArenaRadius, bot.GetExactDist2d(&center));
        float const initialFissureGap = fissure ? bot.GetExactDist2d(fissure) : 0.0f;
        for (std::size_t index = 1; index < points.size(); ++index)
        {
            auto const& from = points[index - 1];
            auto const leg = points[index] - from;
            int const samples = std::max(1, static_cast<int>(std::ceil(leg.length())));
            for (int sample = 1; sample <= samples; ++sample)
            {
                auto const p = from + leg * (static_cast<float>(sample) / samples);
                if (center.GetExactDist2d(p.x, p.y) > outerLimit ||
                    (fissure && fissure->GetExactDist2d(p.x, p.y) + 0.15f <
                        std::min(initialFissureGap, FissureDanger)))
                    return {};
                PositionFullTerrainStatus terrain;
                bot.GetMap()->GetFullTerrainStatusForPosition(bot.GetPhaseMask(), p.x, p.y, p.z + 0.8f,
                                                              bot.GetCollisionHeight(), terrain);
                if (terrain.floorZ <= INVALID_HEIGHT || std::abs(terrain.floorZ - p.z) > 1.0f ||
                    (terrain.liquidInfo.Status & MAP_LIQUID_STATUS_IN_CONTACT))
                    return {};
            }
        }
        return points.size() >= 2 ? points : Movement::PointsArray{};
    }

    Creature const* NearestFissure(Player& bot, Unit const& boss)
    {
        // The warning lasts only two seconds. Don't wait for the generic one-second trigger cache.
        std::list<Creature*> fissures;
        bot.GetCreatureListWithEntryInGrid(fissures, ShadowFissureEntry, FissureDanger);
        Creature const* nearest = nullptr;
        float gap = FissureDanger;
        for (Creature const* fissure : fissures)
        {
            float const distance = bot.GetExactDist2d(fissure);
            if (fissure->IsAlive() && fissure->InSamePhase(&bot) && fissure->IsSummon() &&
                fissure->GetSummonerGUID() == boss.GetGUID() && distance <= gap)
            {
                nearest = fissure;
                gap = distance;
            }
        }
        return nearest;
    }

    bool LaunchEscape(PlayerbotAI& ai, Movement::PointsArray const& path)
    {
        Player& bot = *ai.GetBot();
        if (path.empty() || !ai.CanMove() || bot.GetVehicle())
            return false;

        auto const& end = path.back();
        LastMovement& last = ai.GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get();
        if (bot.isMoving() && !bot.movespline->Finalized() && last.priority == MovementPriority::MOVEMENT_FORCED &&
            (bot.movespline->FinalDestination() - end).squaredLength() < 1.0f)
            return true;

        if (bot.IsSitState())
            bot.SetStandState(UNIT_STAND_STATE_STAND);
        bot.CastStop();
        bot.GetMotionMaster()->Clear();
        last.clear();
        Movement::MoveSplineInit init(&bot);
        init.MovebyPath(path);
        init.SetWalk(false);
        int32 const duration = init.Launch();
        if (duration <= 0)
            return false;
        last.Set(bot.GetMapId(), end.x, end.y, end.z, bot.GetOrientation(),
                 static_cast<float>(duration), MovementPriority::MOVEMENT_FORCED);
        return true;
    }

    Position Project(Player const& bot, Position const& origin, float angle, float distance)
    {
        float const x = origin.GetPositionX() + std::cos(angle) * distance;
        float const y = origin.GetPositionY() + std::sin(angle) * distance;
        return {x, y, bot.GetMapHeight(x, y, bot.GetPositionZ() + 2.0f)};
    }

    bool EscapeFissure(PlayerbotAI& ai, Creature const& fissure, Position const& center)
    {
        Player& bot = *ai.GetBot();
        float const away = fissure.GetAngle(&bot);
        for (float turn : {0.0f, QuarterTurn, -QuarterTurn, 2.0f * QuarterTurn, -2.0f * QuarterTurn})
        {
            Position const end = Project(bot, fissure, away + turn, FissureSafe);
            if (end.GetPositionZ() <= INVALID_HEIGHT)
                continue;
            if (LaunchEscape(ai, CheckedEscape(bot, end, center, 12.0f, &fissure)))
                return true;
        }
        return false;
    }

    bool IsolateMana(PlayerbotAI& ai, Aura const& aura, Position const& center)
    {
        Player& bot = *ai.GetBot();
        Group const* group = bot.GetGroup();
        if (!group || !ai.CanMove() || aura.GetDuration() <= 350)
            return false;

        std::vector<Player const*> nearby;
        Player const* closest = nullptr;
        float closestGap = DetonationSafe;
        for (GroupReference const* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player const* member = ref->GetSource();
            if (!member || member == &bot || !member->IsAlive() || !member->IsInMap(&bot) ||
                !member->InSamePhase(&bot) || bot.GetExactDist2d(member) > 36.0f)
                continue;
            nearby.push_back(member);
            float const gap = bot.GetExactDist2d(member);
            if (gap < closestGap)
            {
                closest = member;
                closestGap = gap;
            }
        }
        if (!closest)
            return false;

        float const maxWalk = bot.GetSpeed(MOVE_RUN) * (aura.GetDuration() - 350) / 1000.0f;
        float const away = closest->GetAngle(&bot);
        for (float distance : {6.0f, 11.0f, 16.0f, 22.0f})
        {
            if (distance > maxWalk)
                break;
            for (float turn : {0.0f, QuarterTurn, -QuarterTurn, 2.0f * QuarterTurn, -2.0f * QuarterTurn,
                               3.0f * QuarterTurn, -3.0f * QuarterTurn, 4.0f * QuarterTurn})
            {
                Position const end = Project(bot, bot, away + turn, distance);
                if (end.GetPositionZ() <= INVALID_HEIGHT ||
                    std::ranges::any_of(nearby, [&](Player const* member)
                    { return end.GetExactDist2d(member) < DetonationSafe; }))
                    continue;
                if (LaunchEscape(ai, CheckedEscape(bot, end, center, maxWalk, nullptr)))
                    return true;
            }
        }
        return false;
    }

    bool AvoidFrozenMana(PlayerbotAI& ai, Unit const& boss, Position const& center)
    {
        Player& bot = *ai.GetBot();
        if (!bot.GetGroup() || !ai.CanMove())
            return false;

        Player const* carrier = nullptr;
        int32 remaining = 0;
        float nearest = DetonationSafe;
        for (GroupReference const* ref = bot.GetGroup()->GetFirstMember(); ref; ref = ref->next())
        {
            Player const* member = ref->GetSource();
            if (!member || member == &bot || !member->IsAlive() || !member->IsInMap(&bot) ||
                !member->InSamePhase(&bot) || !member->HasAura(FrostBlastAura, boss.GetGUID()))
                continue;

            Aura const* mana = member->GetAura(DetonateMana, boss.GetGUID());
            float const gap = bot.GetExactDist2d(member);
            if (mana && mana->GetDuration() > 250 && gap < nearest)
            {
                carrier = member;
                remaining = mana->GetDuration();
                nearest = gap;
            }
        }
        if (!carrier)
            return false;

        // A frozen carrier cannot isolate. Move only enough to clear the blast radius.
        float const away = carrier->GetAngle(&bot);
        float const maxWalk = std::min(14.0f, bot.GetSpeed(MOVE_RUN) * (remaining - 250) / 1000.0f);
        for (float turn : {0.0f, QuarterTurn, -QuarterTurn, 2.0f * QuarterTurn, -2.0f * QuarterTurn})
        {
            Position const end = Project(bot, *carrier, away + turn, DetonationSafe + 0.75f);
            if (end.GetPositionZ() > INVALID_HEIGHT &&
                LaunchEscape(ai, CheckedEscape(bot, end, center, maxWalk, nullptr)))
                return true;
        }
        return false;
    }
}

Aura const* KelthuzadBossHelper::Naxx40ManaDetonation() const
{
    return IsNaxx40Encounter() ? bot->GetAura(DetonateMana, GetBoss()->GetGUID()) : nullptr;
}

bool KelthuzadChooseTargetAction::Execute(Event /*event*/)
{
    if (!helper.UpdateBossAI())
        return false;

    if (helper.IsNaxx40Encounter() && helper.IsPhaseTwo() && HealFrostBlast(*botAI, *helper.GetBoss()))
        return true;

    GuidVector attackers = context->GetValue<GuidVector>("attackers")->Get();
    if (helper.IsNaxx40PhaseOne())
    {
        if (!helper.InNaxx40Camp() || PlayerbotAI::IsHeal(bot))
            return false;

        Unit* incoming = SelectIncoming(*botAI, helper, attackers);
        if (!incoming || AI_VALUE(Unit*, "current target") == incoming)
            return false;

        if (helper.IsNaxx40RangedAdd(incoming) && (bot->IsWithinMeleeRange(incoming) || PlayerbotAI::IsMelee(bot)))
            return SelectWithoutMelee(*bot, *context, incoming);
        return Attack(incoming, false);
    }

    bool const cleanup = helper.IsNaxx40Encounter();
    if (cleanup && PlayerbotAI::IsRanged(bot) && !PlayerbotAI::IsTank(bot) && !PlayerbotAI::IsHeal(bot))
        if (Creature* weaver = SelectEngagedWeaver(*botAI, *helper.GetBoss()))
        {
            if (AI_VALUE(Unit*, "current target") == weaver)
                return false;
            if (bot->IsWithinMeleeRange(weaver))
                return SelectWithoutMelee(*bot, *context, weaver);
            return Attack(weaver, false);
        }

    Unit* target = nullptr;
    Unit *target_soldier = nullptr, *target_weaver = nullptr, *target_abomination = nullptr, *target_kelthuzad = nullptr,
         *target_guardian = nullptr;
    for (auto i = attackers.begin(); i != attackers.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (!unit || (cleanup && helper.IsNaxx40RangedAdd(unit)))
            continue;

        if (botAI->EqualLowercaseName(unit->GetName(), "guardian of icecrown"))
        {
            if (!target_guardian)
                target_guardian = unit;
            else if (unit->GetVictim() && target_guardian->GetVictim() && unit->GetVictim()->ToPlayer() &&
                     target_guardian->GetVictim()->ToPlayer() &&
                     !PlayerbotAI::IsAssistTank(unit->GetVictim()->ToPlayer()) &&
                     PlayerbotAI::IsAssistTank(target_guardian->GetVictim()->ToPlayer()))
            {
                target_guardian = unit;
            }
            else if (unit->GetVictim() && target_guardian->GetVictim() && unit->GetVictim()->ToPlayer() &&
                     target_guardian->GetVictim()->ToPlayer() &&
                     !PlayerbotAI::IsAssistTank(unit->GetVictim()->ToPlayer()) &&
                     !PlayerbotAI::IsAssistTank(target_guardian->GetVictim()->ToPlayer()) &&
                     target_guardian->GetDistance2d(helper.center.first, helper.center.second) >
                         bot->GetDistance2d(unit))
            {
                target_guardian = unit;
            }
        }

        if (unit->GetDistance2d(helper.center.first, helper.center.second) > 30.0f)
            continue;

        if (bot->GetDistance2d(unit) > sPlayerbotAIConfig.SpellDistance)
            continue;

        if (botAI->EqualLowercaseName(unit->GetName(), "unstoppable abomination"))
        {
            if (target_abomination == nullptr ||
                target_abomination->GetDistance2d(helper.center.first, helper.center.second) >
                    unit->GetDistance2d(helper.center.first, helper.center.second))
            {
                target_abomination = unit;
            }
        }
        if (botAI->EqualLowercaseName(unit->GetName(), "soldier of the frozen wastes"))
        {
            if (target_soldier == nullptr ||
                target_soldier->GetDistance2d(helper.center.first, helper.center.second) >
                    unit->GetDistance2d(helper.center.first, helper.center.second))
            {
                target_soldier = unit;
            }
        }
        if (botAI->EqualLowercaseName(unit->GetName(), "soul weaver"))
        {
            if (target_weaver == nullptr || target_weaver->GetDistance2d(helper.center.first, helper.center.second) >
                                                unit->GetDistance2d(helper.center.first, helper.center.second))
                target_weaver = unit;
        }

        if (botAI->EqualLowercaseName(unit->GetName(), "kel'thuzad"))
            target_kelthuzad = unit;
    }
    std::vector<Unit*> targets;
    if (cleanup && target_kelthuzad &&
        (PlayerbotAI::IsMainTank(bot) || (PlayerbotAI::IsTank(bot) && target_kelthuzad->GetVictim() == bot)))
        targets = {target_kelthuzad};
    else if (PlayerbotAI::IsRanged(bot))
    {
        if (botAI->GetRangedDpsIndex(bot) <= 1)
            targets = {target_soldier, target_weaver, target_abomination, target_kelthuzad};
        else
            targets = {target_weaver, target_soldier, target_abomination, target_kelthuzad};
    }
    else if (PlayerbotAI::IsAssistTank(bot))
        targets = {target_abomination, target_guardian, target_kelthuzad};
    else
        targets = {target_abomination, target_kelthuzad};

    for (Unit* t : targets)
    {
        if (t)
        {
            target = t;
            break;
        }
    }
    if (context->GetValue<Unit*>("current target")->Get() == target)
        return false;

    if (target_kelthuzad && target == target_kelthuzad)
        return Attack(target, true);

    return Attack(target, false);
}

bool KelthuzadPositionAction::Execute(Event /*event*/)
{
    if (!helper.UpdateBossAI())
        return false;

    if (helper.IsNaxx40Encounter())
        if (Pet* pet = bot->GetPet(); pet && helper.IsNaxx40RangedAdd(pet->GetVictim()))
        {
            pet->AttackStop();
            pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, PET_FOLLOW_ANGLE);
        }

    if (helper.IsNaxx40PhaseOne())
    {
        bool stopped = false;
        Unit* current = AI_VALUE(Unit*, "current target");
        if (!helper.InNaxx40Camp() || (current && !helper.IsNaxx40Incoming(current)) ||
            (bot->GetVictim() && !helper.IsNaxx40Incoming(bot->GetVictim())))
        {
            stopped = bot->AttackStop() || current;
            if (current)
                context->GetValue<Unit*>("current target")->Set(nullptr);

            for (CurrentSpellTypes slot : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL, CURRENT_AUTOREPEAT_SPELL})
            {
                Spell* spell = bot->GetCurrentSpell(slot);
                if (spell && !spell->GetSpellInfo()->IsPositive())
                {
                    bot->InterruptSpell(slot);
                    stopped = true;
                }
            }
        }
        if (Pet* pet = bot->GetPet(); pet && pet->GetVictim() &&
            (!helper.InNaxx40Camp(pet) || !helper.IsNaxx40Incoming(pet->GetVictim())))
        {
            pet->AttackStop();
            pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, PET_FOLLOW_ANGLE);
            stopped = true;
        }

        int const slot = std::max(0, botAI->GetGroupSlotIndex(bot));
        float const angle = static_cast<float>(slot % 16) * static_cast<float>(M_PI) / 8.0f;
        float const radius = PlayerbotAI::IsTank(bot) ? 3.0f :
            (PlayerbotAI::IsRanged(bot) || PlayerbotAI::IsHeal(bot) ? 8.0f : 5.0f);
        float const x = PlayerbotAI::IsMainTank(bot) ? helper.tank_pos.first : helper.center.first + cos(angle) * radius;
        float const y = PlayerbotAI::IsMainTank(bot) ? helper.tank_pos.second : helper.center.second + sin(angle) * radius;
        float const z = bot->GetMapHeight(x, y, bot->GetPositionZ() + 2.0f);
        if (z <= INVALID_HEIGHT || bot->GetExactDist2d(x, y) <= 2.5f)
            return stopped;

        return MoveTo(NAXX_MAP_ID, x, y, z, false, false, false, false, MovementPriority::MOVEMENT_COMBAT) || stopped;
    }

    if (helper.IsPhaseOne())
    {
        if (AI_VALUE(Unit*, "current target") == nullptr)
            return MoveInside(NAXX_MAP_ID, helper.center.first, helper.center.second, bot->GetPositionZ(), 3.0f,
                              MovementPriority::MOVEMENT_COMBAT);
    }
    else if (helper.IsPhaseTwo())
    {
        if (helper.IsNaxx40Encounter())
        {
            Position const center(helper.center.first, helper.center.second, bot->GetPositionZ());
            if (Creature const* fissure = NearestFissure(*bot, *helper.GetBoss()))
                return EscapeFissure(*botAI, *fissure, center);
            if (Aura const* mana = helper.Naxx40ManaDetonation())
                return IsolateMana(*botAI, *mana, center);
            if (AvoidFrozenMana(*botAI, *helper.GetBoss(), center))
                return true;
        }
        Unit* shadow_fissure = helper.IsNaxx40Encounter() ? nullptr : helper.GetAnyShadowFissure();
        if (!shadow_fissure || !bot->IsWithinDistInMap(shadow_fissure, 10.0f))
        {
            float distance, angle;
            if (PlayerbotAI::IsMainTank(bot))
            {
                if (AI_VALUE2(bool, "has aggro", "current target"))
                    return MoveTo(NAXX_MAP_ID, helper.tank_pos.first, helper.tank_pos.second, bot->GetPositionZ(), false, false, false,
                                  false, MovementPriority::MOVEMENT_COMBAT);
                else
                    return false;
            }
            else if (PlayerbotAI::IsRanged(bot))
            {
                uint32 index = botAI->GetRangedIndex(bot);
                if (index < 8)
                {
                    distance = 20.0f;
                    angle = index * M_PI / 4;
                }
                else
                {
                    distance = 32.0f;
                    angle = (index - 8) * M_PI / 4;
                }
                float dx, dy;
                dx = helper.center.first + cos(angle) * distance;
                dy = helper.center.second + sin(angle) * distance;
                return MoveTo(NAXX_MAP_ID, dx, dy, bot->GetPositionZ(), false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
            }
            else if (PlayerbotAI::IsTank(bot))
            {
                Unit* cur_tar = AI_VALUE(Unit*, "current target");
                if (cur_tar && cur_tar->GetVictim() && cur_tar->GetVictim()->ToPlayer() &&
                    botAI->EqualLowercaseName(cur_tar->GetName(), "guardian of icecrown") &&
                    PlayerbotAI::IsAssistTank(cur_tar->GetVictim()->ToPlayer()))
                {
                    return MoveTo(NAXX_MAP_ID, helper.assist_tank_pos.first, helper.assist_tank_pos.second, bot->GetPositionZ(),
                                  false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
                }
                else
                    return false;
            }
        }
        else
        {
            float dx, dy;
            float angle;
            if (!PlayerbotAI::IsRanged(bot))
                angle = shadow_fissure->GetAngle(helper.center.first, helper.center.second);
            else
                angle = bot->GetAngle(shadow_fissure) + M_PI;

            dx = shadow_fissure->GetPositionX() + cos(angle) * 10.0f;
            dy = shadow_fissure->GetPositionY() + sin(angle) * 10.0f;
            return MoveTo(NAXX_MAP_ID, dx, dy, bot->GetPositionZ(), false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
        }
    }
    return false;
}
