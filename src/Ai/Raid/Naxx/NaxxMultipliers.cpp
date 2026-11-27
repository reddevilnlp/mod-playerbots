/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NaxxMultipliers.h"
#include "ChooseTargetActions.h"
#include "DKActions.h"
#include "DruidActions.h"
#include "DruidBearActions.h"
#include "EncounterHelpers.h"
#include "FollowActions.h"
#include "GenericSpellActions.h"
#include "HunterActions.h"
#include "MageActions.h"
#include "MovementActions.h"
#include "Naxx40Encounter.h"
#include "NaxxActions.h"
#include "NaxxSpellIds.h"
#include "PaladinActions.h"
#include "PetsAction.h"
#include "PriestActions.h"
#include "PvpValues.h"
#include "ReachTargetActions.h"
#include "RogueActions.h"
#include "ScriptedCreature.h"
#include "ShamanActions.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"
#include "UseMeetingStoneAction.h"
#include "WarriorActions.h"
#include "WipeAction.h"

#include <algorithm>

float GrobbulusMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "grobbulus");
    if (!boss)
        return 1.0f;

    if (dynamic_cast<AvoidAoeAction*>(action))
        return PlayerbotAI::IsMainTank(bot) ? 0.0f : 1.0f;

    if (dynamic_cast<CombatFormationMoveAction*>(action))
        return 0.0f;

    return 1.0f;
}

float Maexxna40FlankMultiplier::GetValue(Action* action)
{
    if (!action || action->getName() != "rear flank" || !IsNaxx40Map(bot->FindMap()) || !bot->IsInCombat())
        return 1.0f;

    constexpr uint32 Maexxna40Entry = 351006;
    Unit const* boss = AI_VALUE2(Unit*, "find target", "maexxna");
    if (!boss || boss->GetEntry() != Maexxna40Entry || !boss->IsAlive() || !boss->IsInCombat())
        return 1.0f;

    // The stock flank move picks an unverified point on this room's raised floor.
    // Let melee close normally and keep healers and ranged on their current casting floor.
    return 0.0f;
}

float HeiganDanceMultiplier::GetValue(Action* action)
{
    // Cheap action-type checks first; the encounter state is only looked up for actions we may have to block.
    if (dynamic_cast<HeiganDanceAction*>(action) || dynamic_cast<CurePartyMemberAction*>(action) ||
        dynamic_cast<WipeAction*>(action))
        return 1.0f;

    bool repositions = dynamic_cast<CombatFormationMoveAction*>(action) || dynamic_cast<FleeAction*>(action) ||
                       dynamic_cast<CastDisengageAction*>(action) || dynamic_cast<CastBlinkBackAction*>(action);
    bool moves = dynamic_cast<MovementAction*>(action) || dynamic_cast<CastReachTargetSpellAction*>(action);
    auto* spellAction = dynamic_cast<CastSpellAction*>(action);
    bool timedCast = spellAction && !dynamic_cast<CastMeleeSpellAction*>(action);
    if (!repositions && !moves && !timedCast)
        return 1.0f;

    if (!helper.UpdateBossAI())
        return 1.0f;

    // Generic repositioning must never pull a bot off its safe spot or off the platform.
    if (repositions)
        return 0.0f;

    // Ranged bots on the platform during the slow dance are free to act as usual.
    if (!helper.ShouldDance())
        return 1.0f;

    // Dancing: only the dance moves us (charge/intercept/feral charge included - during the fast dance the boss
    // stands in his Plague Cloud). Everything that is not a cast is fine (target selection, facing, ...).
    if (moves)
        return 0.0f;

    // Casts are allowed while standing on the safe spot with enough time left before the next eruption.
    uint32 spellId = AI_VALUE2(uint32, "spell id", spellAction->getSpell());
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return 1.0f;

    uint32 castTime = spellInfo->CalcCastTime(bot);
    if (spellInfo->IsChanneled())
    {
        int32 duration = spellInfo->GetDuration();
        if (duration > 0)
            castTime += uint32(duration);
    }
    if (castTime == 0)
        return 1.0f;

    return helper.CanStandStillFor(castTime + 500) ? 1.0f : 0.0f;
}

float LoathebGenericMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "loatheb");
    if (!boss)
        return 1.0f;

    context->GetValue<bool>("neglect threat")->Set(true);
    if (botAI->GetState() == BOT_STATE_COMBAT &&
        (dynamic_cast<DpsAssistAction*>(action) || dynamic_cast<TankAssistAction*>(action) ||
         dynamic_cast<CastDebuffSpellOnAttackerAction*>(action) || dynamic_cast<FleeAction*>(action) ||
         dynamic_cast<CombatFormationMoveAction*>(action)))
    {
        return 0.0f;
    }
    if (!dynamic_cast<CastHealingSpellAction*>(action))
        return 1.0f;

    Aura* aura = NaxxSpellIds::GetAnyAura(bot, {NaxxSpellIds::NecroticAura10});
    if (!aura)
    {
        // Fallback to name for custom spell data.
        aura = botAI->GetAura("necrotic aura", bot);
    }
    if (!aura || aura->GetDuration() <= 1500)
        return 1.0f;

    return 0.0f;
}

float ThaddiusGenericMultiplier::GetValue(Action* action)
{
    if (!helper.UpdateBossAI())
        return 1.0f;

    if (dynamic_cast<CombatFormationMoveAction*>(action))
        return 0.0f;
    // pet phase
    if (helper.IsPhasePet() &&
        (dynamic_cast<DpsAssistAction*>(action) || dynamic_cast<TankAssistAction*>(action) ||
         dynamic_cast<CastDebuffSpellOnAttackerAction*>(action) ||
         dynamic_cast<ReachPartyMemberToHealAction*>(action) || dynamic_cast<BuffOnMainTankAction*>(action)))
    {
        return 0.0f;
    }
    // die at the same time
    Unit* target = AI_VALUE(Unit*, "current target");
    Unit* feugen = AI_VALUE2(Unit*, "find target", "feugen");
    Unit* stalagg = AI_VALUE2(Unit*, "find target", "stalagg");
    if (helper.IsPhasePet() && target && feugen && stalagg && target->GetHealthPct() <= 40 &&
        (feugen->GetHealthPct() >= target->GetHealthPct() + 3 || stalagg->GetHealthPct() >= target->GetHealthPct() + 3))
    {
        if (dynamic_cast<CastSpellAction*>(action) && !dynamic_cast<CastHealingSpellAction*>(action))
            return 0.0f;
    }
    // magnetic pull
    // uint32 curr_timer = eventMap->GetTimer();
    // // if (curr_phase == 2 && bot->GetPositionZ() > 312.5f && dynamic_cast<MovementAction*>(action))
    // {
    // if (curr_phase == 2 && (curr_timer % 20000 >= 18000 || curr_timer % 20000 <= 2000) &&
    // dynamic_cast<MovementAction*>(action))
    // {
    //     // MotionMaster *mm = bot->GetMotionMaster();
    //     // mm->Clear();
    //     return 0.0f;
    // }
    // thaddius phase
    // if (curr_phase == 8 && dynamic_cast<FleeAction*>(action))
    // {
    //         return 0.0f;
    // }
    return 1.0f;
}

float SapphironGenericMultiplier::GetValue(Action* action)
{
    if (!helper.UpdateBossAI())
        return 1.0f;

    if (dynamic_cast<CastDeathGripAction*>(action) || dynamic_cast<CombatFormationMoveAction*>(action))
        return 0.0f;

    return 1.0f;
}

float InstructorRazuviousGenericMultiplier::GetValue(Action* action)
{
    if (!helper.UpdateBossAI())
        return 1.0f;

    context->GetValue<bool>("neglect threat")->Set(true);
    if (botAI->GetState() == BOT_STATE_COMBAT &&
        (dynamic_cast<DpsAssistAction*>(action) || dynamic_cast<TankAssistAction*>(action) ||
         dynamic_cast<CastTauntAction*>(action) || dynamic_cast<CastDarkCommandAction*>(action) ||
         dynamic_cast<CastHandOfReckoningAction*>(action) || dynamic_cast<CastGrowlAction*>(action)))
    {
        return 0.0f;
    }
    return 1.0f;
}

float KelthuzadGenericMultiplier::GetValue(Action* action)
{
    if (!helper.UpdateBossAI())
        return 1.0f;

    if (helper.IsNaxx40Encounter() && helper.IsPhaseTwo())
    {
        // Volley was still channeling at the end of the two-second Fissure warning.
        if (dynamic_cast<CastVolleyAction*>(action))
            return 0.0f;
        if (helper.Naxx40ManaDetonation() && !dynamic_cast<KelthuzadPositionAction*>(action) &&
            ((dynamic_cast<MovementAction*>(action) && !dynamic_cast<MeleeAction*>(action)) ||
             dynamic_cast<CastReachTargetSpellAction*>(action) || action->getName() == "dungeon clear assist camp combat"))
            return 0.0f;

        bool const melee = dynamic_cast<MeleeAction*>(action) || dynamic_cast<CastMeleeSpellAction*>(action);
        bool const chases = dynamic_cast<ReachMeleeAction*>(action) || dynamic_cast<CastReachTargetSpellAction*>(action)
            || dynamic_cast<AttackAction*>(action);
        bool const petAttack = dynamic_cast<PetAttackAction*>(action) != nullptr;
        if ((melee || chases || petAttack) && !dynamic_cast<KelthuzadChooseTargetAction*>(action))
        {
            Unit* target = petAttack ? AI_VALUE(Unit*, "current target") : action->GetTarget();
            if (helper.IsNaxx40RangedAdd(target))
                return 0.0f;
        }
    }

    if (helper.IsNaxx40PhaseOne())
    {
        if (dynamic_cast<KelthuzadPositionAction*>(action) || dynamic_cast<KelthuzadChooseTargetAction*>(action) ||
            dynamic_cast<CastHealingSpellAction*>(action) || dynamic_cast<WipeAction*>(action))
            return 1.0f;

        if (dynamic_cast<ReachPartyMemberToHealAction*>(action) || dynamic_cast<ReachPartyMemberToResurrectAction*>(action))
        {
            Unit const* target = action->GetTarget();
            return target && helper.InNaxx40Camp(target) ? 1.0f : 0.0f;
        }

        bool const melee = dynamic_cast<MeleeAction*>(action) || dynamic_cast<CastMeleeSpellAction*>(action);
        if ((dynamic_cast<MovementAction*>(action) && !dynamic_cast<MeleeAction*>(action)) ||
            dynamic_cast<CastReachTargetSpellAction*>(action) || dynamic_cast<PetAttackAction*>(action) ||
            action->getName() == "dungeon clear assist camp combat" || EncounterHelpers::IsAoeThreatAction(bot, action))
            return 0.0f;

        auto* cast = dynamic_cast<CastSpellAction*>(action);
        SpellInfo const* spell = cast ? sSpellMgr->GetSpellInfo(AI_VALUE2(uint32, "spell id", cast->getSpell())) : nullptr;
        Unit* target = melee || cast ? action->GetTarget() : nullptr;
        bool const hostileTarget = target && bot->IsValidAttackTarget(target);
        if (melee || hostileTarget || (spell && !spell->IsPositive()))
        {
            if (spell && !spell->IsPositive() && (spell->IsAffectingArea() || spell->IsTargetingArea() ||
                std::ranges::any_of(spell->Effects, [](SpellEffectInfo const& effect) { return effect.ChainTarget > 1; })))
                return 0.0f;

            if (!hostileTarget)
                target = AI_VALUE(Unit*, "current target");
            if (!helper.InNaxx40Camp() || !helper.IsNaxx40Incoming(target) ||
                (melee && helper.IsNaxx40RangedAdd(target)))
                return 0.0f;
        }
    }

    if ((dynamic_cast<DpsAssistAction*>(action) || dynamic_cast<TankAssistAction*>(action) ||
         dynamic_cast<CastDebuffSpellOnAttackerAction*>(action) || dynamic_cast<FleeAction*>(action)))
    {
        return 0.0f;
    }
    if (helper.IsPhaseOne())
    {
        if (dynamic_cast<CastTotemAction*>(action) || dynamic_cast<CastShadowfiendAction*>(action) ||
            dynamic_cast<CastRaiseDeadAction*>(action) || dynamic_cast<CastFeignDeathAction*>(action) ||
            dynamic_cast<CastInvisibilityAction*>(action) || dynamic_cast<CastVanishAction*>(action) ||
            dynamic_cast<PetAttackAction*>(action))
        {
            return 0.0f;
        }
    }
    if (helper.IsPhaseTwo())
    {
        if (dynamic_cast<CastBlizzardAction*>(action) || dynamic_cast<CastFrostNovaAction*>(action))
            return 0.0f;

    }
    return 1.0f;
}

float AnubrekhanGenericMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "anub'rekhan");
    if (!boss)
        return 1.0f;

    if (Anub40::IsActiveBoss(boss))
    {
        if (dynamic_cast<AnubrekhanPositionAction*>(action) || dynamic_cast<WipeAction*>(action))
            return 1.0f;
        if (Anub40::HasLocustSwarm(*boss) &&
            (dynamic_cast<MovementAction*>(action) || dynamic_cast<CastReachTargetSpellAction*>(action) ||
             dynamic_cast<CastMeleeSpellAction*>(action)))
            return 0.0f;

        if (dynamic_cast<DpsAssistAction*>(action) && IsNaxx40Map(bot->FindMap()))
        {
            constexpr uint32 CryptGuard40Entry = 351082;
            Unit const* target = AI_VALUE(Unit*, "current target");
            if (IsUnavailableCombatTarget(bot, target))
                return 1.0f;

            TempSummon const* summon = target->ToTempSummon();
            bool const isGuard = summon && summon->GetEntry() == CryptGuard40Entry && summon->IsInCombat() &&
                                 summon->GetSummonerGUID() == boss->GetGUID();
            // Leave the encounter's target in place so the next tick can approach it.
            if ((target == boss || isGuard) && bot->IsValidAttackTarget(target) && bot->IsWithinLOSInMap(target))
                return 0.0f;
        }
        return 1.0f;
    }

    if (NaxxSpellIds::HasAnyAura(
            boss, {NaxxSpellIds::LocustSwarm10, NaxxSpellIds::LocustSwarm10Alt, NaxxSpellIds::LocustSwarm25}) ||
        botAI->HasAura("locust swarm", boss))
    {
        if (dynamic_cast<FleeAction*>(action))
            return 0.0f;
    }
    return 1.0f;
}

float FourHorsemenGenericMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "sir zeliek");
    if (!boss)
        return 1.0f;

    context->GetValue<bool>("neglect threat")->Set(true);
    if ((dynamic_cast<DpsAssistAction*>(action) || dynamic_cast<TankAssistAction*>(action)))
        return 0.0f;

    return 1.0f;
}

// float GothikGenericMultiplier::GetValue(Action* action)
// {
//     Unit* boss = AI_VALUE2(Unit*, "find target", "gothik the harvester");
//     if (!boss)
//     {
//         return 1.0f;
//     }
//     BossAI* boss_ai = dynamic_cast<BossAI*>(boss->GetAI());
//     EventMap* eventMap = boss_botAI->GetEvents();
//     uint32 curr_phase = eventMap->GetPhaseMask();
//     if (curr_phase == 1 && (dynamic_cast<FollowAction*>(action)))
//     {
//         return 0.0f;
//     }
//     if (curr_phase == 1 && (dynamic_cast<AttackAction*>(action)))
//     {
//         Unit* target = action->GetTarget();
//         if (target == boss)
//         {
//             return 0.0f;
//         }
//     }
//     return 1.0f;
// }

float GluthGenericMultiplier::GetValue(Action* action)
{
    if (!helper.UpdateBossAI())
        return 1.0f;

    if ((dynamic_cast<DpsAssistAction*>(action) || dynamic_cast<TankAssistAction*>(action) ||
         dynamic_cast<FleeAction*>(action) || dynamic_cast<CastDebuffSpellOnAttackerAction*>(action) ||
         dynamic_cast<CastStarfallAction*>(action)))
    {
        return 0.0f;
    }

    if (PlayerbotAI::IsMainTank(bot))
    {
        Aura* aura = NaxxSpellIds::GetAnyAura(bot, {NaxxSpellIds::MortalWound10, NaxxSpellIds::MortalWound25});
        if (!aura)
        {
            // Fallback to name for custom spell data.
            aura = botAI->GetAura("mortal wound", bot, false, true);
        }
        if (aura && aura->GetStackAmount() >= 5)
        {
            if (dynamic_cast<CastTauntAction*>(action) || dynamic_cast<CastDarkCommandAction*>(action) ||
                dynamic_cast<CastHandOfReckoningAction*>(action) || dynamic_cast<CastGrowlAction*>(action))
            {
                return 0.0f;
            }
        }
    }
    if (dynamic_cast<PetAttackAction*>(action))
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        if (helper.IsZombieChow(target))
            return 0.0f;
    }
    return 1.0f;
}
