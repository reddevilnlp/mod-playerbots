/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Gluth's positioning, tank assignments and Decimate response are adapted from electiveDev's Naxx40 strategy.
 * https://github.com/electiveDev/playerbots-custom-strategies/commit/58a5b7b5ed760d2236f894cf9b535d2c18e7bc69
 */

#include "Gluth40.h"

#include "Ai/Raid/RaidClear/Common/BossLookup.h"
#include "Ai/Raid/RaidClear/Common/TankRoles.h"
#include "AttackAction.h"
#include "ChooseTargetActions.h"
#include "Creature.h"
#include "Event.h"
#include "Group.h"
#include "MovementActions.h"
#include "Naxx40Encounter.h"
#include "Playerbots.h"
#include "SpellAuras.h"
#include "Strategy.h"

#include <list>

namespace
{
    constexpr uint32 NPC_GLUTH = 351004;
    constexpr uint32 NPC_ZOMBIE_CHOW = 351069;
    constexpr uint32 SPELL_MORTAL_WOUND = 25646;
    constexpr uint8 SWAP_STACKS = 4;
    constexpr float ROOM_TOP = 300.0f;
    constexpr float CHOW_SEARCH_RANGE = 110.0f;
    constexpr float DECIMATED_HEALTH = 10.0f;
    constexpr float CHOW_DANGER_RANGE = 22.0f;
    Position const BOSS_SPOT { 3331.48f, -3109.06f, 297.42f };
    // Bring the chow camp in from the original spot so healers in the middle can reach both sides.
    Position const CHOW_SPOT { 3282.3f, -3157.7f, 297.42f };
    Position const HEAL_SPOT { 3306.89f, -3133.38f, 297.42f };

    Creature* FightingGluth(Player* bot)
    {
        if (!bot->IsInCombat() || bot->GetPositionZ() > ROOM_TOP || !IsNaxx40Map(bot->FindMap()))
            return nullptr;

        Creature* boss = RaidClear::FightingCreature(bot, { NPC_GLUTH });
        return boss && bot->GetDistance(boss) <= CHOW_SEARCH_RANGE ? boss : nullptr;
    }

    enum class TankRole : uint8 { Main, Backup, Chow, None };

    TankRole GetTankRole(Player* player)
    {
        if (!player || !PlayerbotAI::IsTank(player))
            return TankRole::None;
        if (PlayerbotAI::IsMainTank(player))
            return TankRole::Main;
        return PlayerbotAI::IsAssistTankOfIndex(player, 0, true) ? TankRole::Backup : TankRole::Chow;
    }

    uint8 WoundStacks(Unit const* target)
    {
        Aura const* wound = target ? target->GetAura(SPELL_MORTAL_WOUND) : nullptr;
        return wound ? wound->GetStackAmount() : 0;
    }

    Player* NextBossTank(Player* bot, Creature* boss)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return nullptr;

        Unit* victim = boss->GetVictim();
        if (victim && GetTankRole(victim->ToPlayer()) != TankRole::None && WoundStacks(victim) < SWAP_STACKS)
            return nullptr;

        Player* next = nullptr;
        TankRole bestRole = TankRole::None;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == victim || !member->IsAlive() || member->FindMap() != bot->FindMap() ||
                member->GetPositionZ() > ROOM_TOP || member->GetDistance(boss) > 30.0f || WoundStacks(member) > 1)
                continue;

            TankRole role = GetTankRole(member);
            if (role < bestRole)
            {
                bestRole = role;
                next = member;
            }
        }
        return next;
    }

    enum class ChowTarget : uint8 { Decimated, Loose, Held };

    Creature* FindChow(Player* bot, Creature* boss, ChowTarget kind, float range)
    {
        // Fresh summons attack Gluth first. Scan the room so a chow can be intercepted before it engages a player.
        std::list<Creature*> zombies;
        boss->GetCreatureListWithEntryInGrid(zombies, NPC_ZOMBIE_CHOW, CHOW_SEARCH_RANGE);
        Creature* best = nullptr;
        float bestDistance = CHOW_SEARCH_RANGE;
        for (Creature* zombie : zombies)
        {
            if (!zombie->IsAlive() || bot->GetDistance(zombie) > range || !bot->IsWithinLOSInMap(zombie))
                continue;

            bool decimated = zombie->GetHealthPct() <= DECIMATED_HEALTH;
            Unit* victim = zombie->GetVictim();
            if ((kind == ChowTarget::Decimated && !decimated) || (kind == ChowTarget::Held && victim != bot) ||
                (kind == ChowTarget::Loose &&
                    (decimated || (victim && GetTankRole(victim->ToPlayer()) == TankRole::Chow))))
                continue;

            float distance = kind == ChowTarget::Held ? bot->GetDistance(zombie) : boss->GetDistance(zombie);
            if (distance < bestDistance)
            {
                best = zombie;
                bestDistance = distance;
            }
        }
        return best;
    }

    class Gluth40FightTrigger : public Trigger
    {
    public:
        explicit Gluth40FightTrigger(PlayerbotAI* ai) : Trigger(ai, "gluth40 fight") {}
        bool IsActive() override { return FightingGluth(bot) != nullptr; }
    };

    class Gluth40TargetAction : public AttackAction
    {
    public:
        explicit Gluth40TargetAction(PlayerbotAI* ai) : AttackAction(ai, "gluth40 choose target") {}

        bool Execute(Event /*event*/) override
        {
            Creature* boss = FightingGluth(bot);
            if (!boss || PlayerbotAI::IsHeal(bot))
                return false;

            TankRole role = GetTankRole(bot);
            Unit* target = boss;
            Unit* current = AI_VALUE(Unit*, "current target");
            if (role == TankRole::Chow && boss->GetVictim() != bot)
            {
                target = FindChow(bot, boss, ChowTarget::Decimated, 55.0f);
                if (!target)
                    target = FindChow(bot, boss, ChowTarget::Loose, 55.0f);
                if (!target)
                    target = FindChow(bot, boss, ChowTarget::Held, 15.0f);

                // Do not let the previous target make the add tank leave the chow lane before the next wave.
                if (!target && current == boss)
                {
                    bot->AttackStop();
                    bot->SetSelection(ObjectGuid::Empty);
                    context->GetValue<Unit*>("current target")->Set(nullptr);
                    return true;
                }
            }
            else if (role == TankRole::None)
            {
                Creature* chow = FindChow(bot, boss, ChowTarget::Decimated, 60.0f);
                if (!chow && PlayerbotAI::IsRangedDps(bot))
                {
                    Creature* loose = FindChow(bot, boss, ChowTarget::Loose, botAI->GetRange("spell"));
                    if (loose && boss->GetDistance(loose) < CHOW_DANGER_RANGE)
                        chow = loose;
                }
                if (chow)
                    target = chow;
            }

            return target && target != current && Attack(target);
        }
    };

    class Gluth40PositionAction : public MovementAction
    {
    public:
        explicit Gluth40PositionAction(PlayerbotAI* ai) : MovementAction(ai, "gluth40 position") {}

        bool Execute(Event /*event*/) override
        {
            Creature* boss = FightingGluth(bot);
            if (!boss)
                return false;
            if (PlayerbotAI::IsHeal(bot))
                return PositionHealer(boss);

            TankRole role = GetTankRole(bot);
            if (role == TankRole::None)
                return false;
            if (boss->GetVictim() == bot)
                return HoldSpot(BOSS_SPOT, 3.0f);
            if (role != TankRole::Chow)
                return false;

            Unit* target = AI_VALUE(Unit*, "current target");
            if (target && target->IsAlive() && target->GetEntry() == NPC_ZOMBIE_CHOW &&
                (target->GetVictim() != bot || target->GetHealthPct() <= DECIMATED_HEALTH))
                return false;

            return HoldSpot(CHOW_SPOT, 5.0f);
        }

    private:
        bool HoldSpot(Position const& spot, float radius)
        {
            return MoveInside(bot->GetMapId(), spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), radius,
                MovementPriority::MOVEMENT_COMBAT);
        }

        bool PositionHealer(Creature* boss)
        {
            if (boss->GetDistance(BOSS_SPOT) > 12.0f || bot->GetHealthPct() < 35.0f ||
                bot->HasUnitState(UNIT_STATE_CASTING))
                return false;

            if (Unit* target = AI_VALUE(Unit*, "party member to heal"))
                if (target->GetHealthPct() < 35.0f ||
                    target->GetExactDist(HEAL_SPOT) > botAI->GetRange("heal") - 2.0f)
                    return false;

            return HoldSpot(HEAL_SPOT, 2.0f);
        }
    };

    class Gluth40TauntAction : public Action
    {
    public:
        explicit Gluth40TauntAction(PlayerbotAI* ai) : Action(ai, "gluth40 taunt") {}

        bool Execute(Event /*event*/) override
        {
            Creature* boss = FightingGluth(bot);
            TankRole role = GetTankRole(bot);
            if (!boss || role == TankRole::None || boss->GetVictim() == bot)
                return false;

            if (NextBossTank(bot, boss) == bot && RaidClear::Tanks::Taunt(botAI, boss))
                return true;
            if (role != TankRole::Chow)
                return false;

            Creature* chow = FindChow(bot, boss, ChowTarget::Loose, 30.0f);
            return chow && RaidClear::Tanks::Taunt(botAI, chow);
        }
    };

    class Gluth40ControlAction : public Action
    {
    public:
        explicit Gluth40ControlAction(PlayerbotAI* ai) : Action(ai, "gluth40 control") {}

        bool Execute(Event /*event*/) override
        {
            Creature* boss = FightingGluth(bot);
            if (!boss || PlayerbotAI::IsHeal(bot))
                return false;

            switch (bot->getClass())
            {
                case CLASS_MAGE:
                    return FindChow(bot, boss, ChowTarget::Decimated, 10.0f) && botAI->CastSpell("frost nova", bot);
                case CLASS_HUNTER:
                    return FindChow(bot, boss, ChowTarget::Decimated, 10.0f) && botAI->CastSpell("frost trap", bot);
                case CLASS_PALADIN:
                    return FindChow(bot, boss, ChowTarget::Decimated, 10.0f) && botAI->CastSpell("holy wrath", bot);
                case CLASS_WARRIOR:
                    return FindChow(bot, boss, ChowTarget::Decimated, 10.0f) && botAI->CastSpell("piercing howl", bot);
                case CLASS_WARLOCK:
                    if (Creature* chow = FindChow(bot, boss, ChowTarget::Decimated, 30.0f))
                        return botAI->CastSpell("shadowfury", chow);
                    break;
                default:
                    break;
            }
            return false;
        }
    };

    class Gluth40Multiplier : public Multiplier
    {
    public:
        explicit Gluth40Multiplier(PlayerbotAI* ai) : Multiplier(ai, "gluth40 target discipline") {}

        float GetValue(Action* action) override
        {
            if (!action)
                return 1.0f;

            bool assist = dynamic_cast<DpsAssistAction*>(action) || dynamic_cast<TankAssistAction*>(action) ||
                dynamic_cast<RcOffTankTargetAction*>(action) || dynamic_cast<RcOffTankSeparateAction*>(action) ||
                dynamic_cast<RcOffTankStackAction*>(action);
            bool taunt = RaidClear::Tanks::IsTauntAction(action);
            if (!assist && !taunt)
                return 1.0f;

            Creature* boss = FightingGluth(bot);
            if (!boss)
                return 1.0f;
            if (assist && !PlayerbotAI::IsHeal(bot))
                return 0.0f;
            if (taunt)
            {
                Unit* target = action->GetTarget();
                if (target == boss ||
                    (target && target == boss->GetVictim() && action->getName() == "righteous defense"))
                    return 0.0f;
            }
            return 1.0f;
        }
    };
}

Gluth40ActionContext::Gluth40ActionContext()
{
    creators["gluth40 choose target"] = [](PlayerbotAI* ai) { return new Gluth40TargetAction(ai); };
    creators["gluth40 position"] = [](PlayerbotAI* ai) { return new Gluth40PositionAction(ai); };
    creators["gluth40 taunt"] = [](PlayerbotAI* ai) { return new Gluth40TauntAction(ai); };
    creators["gluth40 control"] = [](PlayerbotAI* ai) { return new Gluth40ControlAction(ai); };
}

Gluth40TriggerContext::Gluth40TriggerContext()
{
    creators["gluth40 fight"] = [](PlayerbotAI* ai) { return new Gluth40FightTrigger(ai); };
}

void AddGluth40Triggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("gluth40 fight", {
        NextAction("gluth40 taunt", ACTION_RAID + 4),
        NextAction("gluth40 control", ACTION_RAID + 3),
        NextAction("gluth40 position", ACTION_RAID + 2),
        NextAction("gluth40 choose target", ACTION_RAID + 1),
    }));
}

void AddGluth40Multipliers(PlayerbotAI* botAI, std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new Gluth40Multiplier(botAI));
}
