/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NaxxActions.h"
#include "Group.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Pet.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"

#include <algorithm>
#include <array>

namespace
{
constexpr float SoldierInterceptRadius = 20.0f;
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

Unit* SelectIncoming(PlayerbotAI& ai, KelthuzadBossHelper const& helper, GuidVector const& attackers,
                     bool rangedAddsOnly)
{
    Unit* incoming = nullptr;
    int priority = 0;
    float distance = 0.0f;
    for (ObjectGuid const& guid : attackers)
    {
        Unit* candidate = ai.GetUnit(guid);
        if ((rangedAddsOnly && !helper.IsNaxx40RangedAdd(candidate)) || !helper.IsNaxx40Incoming(candidate))
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

bool SelectWithoutMelee(Player& bot, AiObjectContext& context, Unit* target)
{
    bot.AttackStop();
    bot.SetSelection(target->GetGUID());
    context.GetValue<Unit*>("current target")->Set(target);
    return true;
}
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

        Unit* incoming = SelectIncoming(*botAI, helper, attackers, false);
        if (!incoming || AI_VALUE(Unit*, "current target") == incoming)
            return false;

        if (helper.IsNaxx40RangedAdd(incoming) && (bot->IsWithinMeleeRange(incoming) || PlayerbotAI::IsMelee(bot)))
            return SelectWithoutMelee(*bot, *context, incoming);
        return Attack(incoming, false);
    }

    bool const cleanup = helper.IsNaxx40Encounter();
    if (cleanup && PlayerbotAI::IsRanged(bot) && !PlayerbotAI::IsTank(bot) && !PlayerbotAI::IsHeal(bot))
        if (Unit* incoming = SelectIncoming(*botAI, helper, attackers, true))
        {
            if (AI_VALUE(Unit*, "current target") == incoming)
                return false;
            if (bot->IsWithinMeleeRange(incoming) || PlayerbotAI::IsMelee(bot))
                return SelectWithoutMelee(*bot, *context, incoming);
            return Attack(incoming, false);
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
    if (cleanup && PlayerbotAI::IsMainTank(bot) && target_kelthuzad)
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
        Unit* shadow_fissure = helper.GetAnyShadowFissure();
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
