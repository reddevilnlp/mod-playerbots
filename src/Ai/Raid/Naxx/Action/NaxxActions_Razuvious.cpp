/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NaxxActions.h"

#include "../../RaidClear/Common/WayOut.h"
#include "Creature.h"
#include "Multiplier.h"
#include "Naxx40Encounter.h"
#include "ObjectGuid.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Razuvious40.h"
#include "ReachTargetActions.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "Strategy.h"
#include "Trigger.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr uint32 SPELL_UNDERSTUDY_TAUNT = 29060;
constexpr uint32 SPELL_BONE_BARRIER     = 29061;
constexpr uint32 SPELL_BLOOD_STRIKE     = 61696;
constexpr uint32 SPELL_MIND_CONTROL     = 605;
constexpr uint32 NPC_RAZUVIOUS_40       = 351036;
constexpr uint32 NPC_UNDERSTUDY_40      = 351084;

// Mana Burn checks distance to the caster's center. Keep some room for movement.
constexpr float MANA_BURN_KEEP_AWAY = 31.5f;
constexpr float SAFE_SPOT_SCORE = 32.0f;
constexpr float USEFUL_SPOT_SCORE = 35.0f;
constexpr char ManaBurnTrigger[] = "razuvious40 mana burn positioning";
constexpr char ManaBurnAction[] = "razuvious40 mana burn position";

bool KeepsManaBurnDistance(PlayerbotAI& ai)
{
    Player* bot = ai.GetBot();
    Spell const* cast = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    Spell const* channel = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
    return bot->IsAlive() && bot->IsInCombat() && IsNaxx40Map(bot->FindMap()) && !bot->GetCharm() &&
        (!cast || cast->GetSpellInfo()->Id != SPELL_MIND_CONTROL) &&
        (!channel || channel->GetSpellInfo()->Id != SPELL_MIND_CONTROL) &&
        bot->getPowerType() == POWER_MANA && !PlayerbotAI::IsTank(bot) &&
        (PlayerbotAI::IsHeal(bot) || PlayerbotAI::IsRanged(bot));
}

Creature* FightingRazuvious(PlayerbotAI& ai)
{
    Unit* unit = ai.GetAiObjectContext()->GetValue<Unit*>("find target", "instructor razuvious")->Get();
    if (!unit || unit->GetEntry() != NPC_RAZUVIOUS_40 || !unit->IsAlive() || !unit->IsInCombat())
        return nullptr;
    return unit->ToCreature();
}

struct RangeGoal
{
    Unit* target;
    float range;
};

float RangedAbilityRange(PlayerbotAI& ai, Unit& target)
{
    Player* bot = ai.GetBot();
    char const* spellName;
    switch (bot->getClass())
    {
        case CLASS_DRUID: spellName = "wrath"; break;
        case CLASS_HUNTER: spellName = "auto shot"; break;
        case CLASS_MAGE: spellName = "frostbolt"; break;
        case CLASS_PRIEST: spellName = "mind blast"; break;
        case CLASS_SHAMAN: spellName = "lightning bolt"; break;
        case CLASS_WARLOCK: spellName = "shadow bolt"; break;
        default: return ai.GetRange("spell");
    }

    uint32 const id = ai.GetAiObjectContext()->GetValue<uint32>("spell id", spellName)->Get();
    SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
    return info ? std::max(ai.GetRange("spell"), bot->GetSpellMaxRangeForTarget(&target, info)) :
        ai.GetRange("spell");
}

RangeGoal GetRangeGoal(PlayerbotAI& ai, Creature& boss)
{
    Player* bot = ai.GetBot();
    AiObjectContext* context = ai.GetAiObjectContext();
    if (PlayerbotAI::IsHeal(bot))
    {
        Unit* target = context->GetValue<Unit*>("party member to heal")->Get();
        if (!target || target == bot || !target->IsAlive() || !target->IsInMap(bot))
            target = boss.GetVictim();
        if (!target || target == bot || !target->IsAlive() || !bot->IsFriendlyTo(target))
            target = &boss;
        return {target, ai.GetRange("heal")};
    }

    Unit* target = context->GetValue<Unit*>("current target")->Get();
    if (!target || !target->IsAlive() || !target->IsInMap(bot) || !target->IsInCombat() ||
        !bot->IsValidAttackTarget(target))
        target = &boss;
    return {target, RangedAbilityRange(ai, *target)};
}

bool CanWorkFrom(Player const& bot, RangeGoal const& goal, float x, float y, float z)
{
    float const reach = goal.range + bot.GetCombatReach() + goal.target->GetCombatReach();
    return goal.target->GetExactDist(x, y, z) <= reach && goal.target->IsWithinLOS(x, y, z);
}

class Razuvious40PositionTrigger final : public Trigger
{
public:
    explicit Razuvious40PositionTrigger(PlayerbotAI* ai) : Trigger(ai, ManaBurnTrigger) {}

    bool IsActive() override
    {
        return KeepsManaBurnDistance(*botAI) && botAI->CanMove() &&
            !botAI->HasStrategy("stay", botAI->GetState()) && FightingRazuvious(*botAI);
    }
};

class Razuvious40PositionAction final : public Action
{
public:
    explicit Razuvious40PositionAction(PlayerbotAI* ai) : Action(ai, ManaBurnAction), _walk(ai) {}

    bool Execute(Event /*event*/) override
    {
        if (!KeepsManaBurnDistance(*botAI) || !botAI->CanMove() || botAI->HasStrategy("stay", botAI->GetState()))
            return false;
        Creature* boss = FightingRazuvious(*botAI);
        if (!boss || (_walk.NoSpotAtMs() && GetMSTimeDiffToNow(_walk.NoSpotAtMs()) < 1500))
            return false;

        RangeGoal const goal = GetRangeGoal(*botAI, *boss);
        auto const score = [&](float x, float y, float z)
        {
            float const distance = boss->GetExactDist(x, y, z);
            if (distance < MANA_BURN_KEEP_AWAY)
                return distance;

            return CanWorkFrom(*bot, goal, x, y, z) ? USEFUL_SPOT_SCORE : SAFE_SPOT_SCORE;
        };

        if (score(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()) == USEFUL_SPOT_SCORE)
        {
            if (bot->isMoving() && !bot->movespline->Finalized())
            {
                G3D::Vector3 const end = bot->movespline->FinalDestination();
                if (boss->GetExactDist(end.x, end.y, end.z) < MANA_BURN_KEEP_AWAY)
                {
                    bot->GetMotionMaster()->Clear();
                    bot->StopMoving();
                    AI_VALUE(LastMovement&, "last movement").clear();
                    return true;
                }
            }
            return false;
        }

        return _walk.Go(score, SAFE_SPOT_SCORE, USEFUL_SPOT_SCORE, 1.0f, 36.0f);
    }

private:
    RaidClear::WayOutWalk _walk;
};

class Razuvious40PositionMultiplier final : public Multiplier
{
public:
    explicit Razuvious40PositionMultiplier(PlayerbotAI* ai) : Multiplier(ai, "razuvious40 mana burn reach") {}

    float GetValue(Action* action) override
    {
        std::string const name = action->getName();
        bool const heals = name == "reach party member to heal";
        if ((!heals && name != "reach spell") || !KeepsManaBurnDistance(*botAI))
            return 1.0f;
        Creature* boss = FightingRazuvious(*botAI);
        Unit* target = action->GetTarget();
        if (!boss || !target || target == bot)
            return 1.0f;

        float const reach = botAI->GetRange(heals ? "heal" : "spell") +
            bot->GetCombatReach() + target->GetCombatReach();
        float const distance = bot->GetExactDist(target);
        float const travel = distance - reach;
        if (travel <= 0.0f)
            return 1.0f;

        // The stock reach action shortens its first leg for targets that are further away.
        float const fraction = (travel >= 6.0f ? travel * 0.5f : travel) / distance;
        float const x = std::lerp(bot->GetPositionX(), target->GetPositionX(), fraction);
        float const y = std::lerp(bot->GetPositionY(), target->GetPositionY(), fraction);
        float const z = std::lerp(bot->GetPositionZ(), target->GetPositionZ(), fraction);
        return boss->GetExactDist(x, y, z) < MANA_BURN_KEEP_AWAY ? 0.0f : 1.0f;
    }
};
}

bool DirectRazuvious40Understudy(PlayerbotAI& ai, Creature& boss)
{
    Player* priest = ai.GetBot();
    Unit* charm = priest->GetCharm();
    if (!IsNaxx40Map(priest->FindMap()) || !priest->IsAlive() || boss.GetEntry() != NPC_RAZUVIOUS_40 ||
        !boss.IsAlive() || !boss.IsInMap(priest) || !charm || charm->GetEntry() != NPC_UNDERSTUDY_40 ||
        !charm->IsAlive() || !charm->GetAI())
        return false;
    Aura const* control = charm->GetAura(SPELL_MIND_CONTROL, priest->GetGUID());
    if (!control)
        return false;

    Unit* victim = boss.GetVictim();
    Aura const* victimBarrier = victim ? victim->GetAura(SPELL_BONE_BARRIER) : nullptr;
    Aura const* ownBarrier = charm->GetAura(SPELL_BONE_BARRIER);
    bool const needsProtectedTank = !victimBarrier || victimBarrier->GetDuration() <= 1500;
    bool const canShield = (ownBarrier && ownBarrier->GetDuration() > 1500) ||
        !charm->HasSpellCooldown(SPELL_BONE_BARRIER);
    SpellInfo const* taunt = sSpellMgr->GetSpellInfo(SPELL_UNDERSTUDY_TAUNT);
    bool const canTaunt = needsProtectedTank && canShield && control->GetDuration() > 3000 && taunt &&
        !charm->HasSpellCooldown(SPELL_UNDERSTUDY_TAUNT);
    bool acted = false;

    if (canTaunt && charm->IsWithinCombatRange(&boss, taunt->GetMaxRange(false, charm)) &&
        charm->IsWithinLOSInMap(&boss))
    {
        if (!ownBarrier || ownBarrier->GetDuration() <= 1500)
        {
            charm->CastSpell(charm, SPELL_BONE_BARRIER, true);
            charm->AddSpellCooldown(SPELL_BONE_BARRIER, 0, 30 * IN_MILLISECONDS);
        }

        if (charm->HasAura(SPELL_BONE_BARRIER))
        {
            charm->CastSpell(&boss, SPELL_UNDERSTUDY_TAUNT, true);
            charm->AddSpellCooldown(SPELL_UNDERSTUDY_TAUNT, 0, 20 * IN_MILLISECONDS);
            charm->GetAI()->AttackStart(&boss);
            if (charm->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_ACTIVE) != CHASE_MOTION_TYPE)
                charm->GetMotionMaster()->MoveChase(&boss);
            acted = true;
        }
    }
    else if (charm->GetVictim() != &boss)
    {
        if (boss.IsInCombat())
        {
            charm->GetAI()->AttackStart(&boss);
            charm->GetMotionMaster()->MoveChase(&boss);
            acted = true;
        }
        else if (canTaunt && charm->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_ACTIVE) != POINT_MOTION_TYPE)
        {
            // Before pull, walk the add into range without turning on its melee attacks.
            Position const point = boss.GetNearPosition(3.0f, boss.GetRelativeAngle(charm));
            charm->GetMotionMaster()->MovePoint(0, point, FORCED_MOVEMENT_RUN, 0.0f, true, false);
            return true;
        }
    }

    if (boss.IsInCombat() && charm->IsWithinMeleeRange(&boss) && !charm->HasSpellCooldown(SPELL_BLOOD_STRIKE))
    {
        charm->CastSpell(&boss, SPELL_BLOOD_STRIKE, true);
        charm->AddSpellCooldown(SPELL_BLOOD_STRIKE, 0, 4 * IN_MILLISECONDS);
        acted = true;
    }
    return acted;
}

Action* CreateRazuvious40PositionAction(PlayerbotAI* ai)
{
    return new Razuvious40PositionAction(ai);
}

Trigger* CreateRazuvious40PositionTrigger(PlayerbotAI* ai)
{
    return new Razuvious40PositionTrigger(ai);
}

void AddRazuvious40Triggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(ManaBurnTrigger, {NextAction(ManaBurnAction, ACTION_RAID + 4)}));
}

void AddRazuvious40Multipliers(PlayerbotAI* ai, std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new Razuvious40PositionMultiplier(ai));
}

bool RazuviousUseObedienceCrystalAction::Execute(Event /*event*/)
{
    if (!helper.UpdateBossAI())
        return false;

    // bot->GetCharm
    if (Unit* charm = bot->GetCharm())
    {
        Unit* target = AI_VALUE2(Unit*, "find target", "instructor razuvious");
        if (!target)
            return false;

        if (IsNaxx40Map(bot->FindMap()))
            return target->ToCreature() && DirectRazuvious40Understudy(*botAI, *target->ToCreature());

        if (charm->GetMotionMaster()->GetMotionSlotType(MOTION_SLOT_ACTIVE) == NULL_MOTION_TYPE)
        {
            charm->GetMotionMaster()->Clear();
            charm->GetMotionMaster()->MoveChase(target);
            charm->GetAI()->AttackStart(target);
        }
        Aura* forceObedience = botAI->GetAura("force obedience", charm);
        uint32 duration_time;
        if (!forceObedience)
        {
            forceObedience = botAI->GetAura("mind control", charm);
            duration_time = 60000;
        }
        else
            duration_time = 90000;

        if (!forceObedience)
            return false;

        if (charm->GetDistance(target) <= 0.51f)
        {
            // taunt
            bool tauntUseful = true;
            if (forceObedience->GetDuration() <= int32(duration_time - 5000))
            {
                Unit* victim = target->GetVictim();
                if (victim && victim->HasAura(SPELL_BONE_BARRIER))
                    tauntUseful = false;

                if (forceObedience->GetDuration() <= 3000)
                    tauntUseful = false;

            }
            if (forceObedience->GetDuration() >= int32(duration_time - 500))
                tauntUseful = false;

            if (tauntUseful && !charm->HasSpellCooldown(29060))
            {
                // shield
                if (!charm->HasSpellCooldown(SPELL_BONE_BARRIER))
                {
                    charm->CastSpell(charm, SPELL_BONE_BARRIER, true);
                    charm->AddSpellCooldown(SPELL_BONE_BARRIER, 0, 30 * IN_MILLISECONDS);
                }
                charm->CastSpell(target, SPELL_UNDERSTUDY_TAUNT, true);
                charm->AddSpellCooldown(SPELL_UNDERSTUDY_TAUNT, 0, 20 * IN_MILLISECONDS);
            }
            // strike
            if (!charm->HasSpellCooldown(SPELL_BLOOD_STRIKE))
            {
                charm->CastSpell(target, SPELL_BLOOD_STRIKE, true);
                charm->AddSpellCooldown(SPELL_BLOOD_STRIKE, 0, 4 * IN_MILLISECONDS);
            }
        }
    }
    else
    {
        Difficulty diff = bot->GetRaidDifficulty();
        if (diff == RAID_DIFFICULTY_10MAN_NORMAL)
        {
            GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
            for (auto i = npcs.begin(); i != npcs.end(); i++)
            {
                Creature* unit = botAI->GetCreature(*i);
                if (!unit)
                    continue;

                if (PlayerbotAI::IsMainTank(bot) && unit->GetSpawnId() != 128352)
                    continue;

                if (!PlayerbotAI::IsMainTank(bot) && unit->GetSpawnId() != 128353)
                    continue;

                if (MoveTo(unit, 0.0f, MovementPriority::MOVEMENT_COMBAT))
                    return true;

                Creature* creature = bot->GetNPCIfCanInteractWith(*i, UNIT_NPC_FLAG_SPELLCLICK);
                if (!creature)
                    continue;

                creature->HandleSpellClick(bot);
                return true;
            }
        }
        else
        {
            GuidVector attackers = context->GetValue<GuidVector>("attackers")->Get();
            Unit* target = nullptr;
            for (auto i = attackers.begin(); i != attackers.end(); ++i)
            {
                Unit* unit = botAI->GetUnit(*i);
                if (!unit)
                    continue;
                if (botAI->EqualLowercaseName(unit->GetName(), "death knight understudy"))
                {
                    target = unit;
                    break;
                }
            }
            if (target)
            {
                if (bot->GetDistance2d(target) > sPlayerbotAIConfig.SpellDistance)
                    return MoveNear(target, sPlayerbotAIConfig.SpellDistance, MovementPriority::MOVEMENT_COMBAT);
                else
                    return botAI->CastSpell("mind control", target);
            }
        }
    }
    return false;
}

bool RazuviousTargetAction::Execute(Event /*event*/)
{
    if (!helper.UpdateBossAI())
        return false;

    Unit* razuvious = AI_VALUE2(Unit*, "find target", "instructor razuvious");
    Unit* understudy = AI_VALUE2(Unit*, "find target", "death knight understudy");
    Unit* target = nullptr;
    if (PlayerbotAI::IsTank(bot))
        target = understudy;
    else
        target = razuvious;

    if (AI_VALUE(Unit*, "current target") == target)
        return false;

    return Attack(target);
}
