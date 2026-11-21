/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NAXX40_HORSEMEN_H
#define PLAYERBOTS_NAXX40_HORSEMEN_H

#include "AttackAction.h"
#include "MovementActions.h"
#include "Multiplier.h"
#include "Trigger.h"

namespace Naxx40Horsemen
{
enum class Tactic
{
    Assign,
    Taunt,
    Corner,
    Marks,
    Meteor,
    VoidZone,
    HolyWrath,
    Attack
};

char const* Name(Tactic tactic);
bool IsNaxx40(Player* player);
void InitTriggers(std::vector<TriggerNode*>& triggers);

template <Tactic tactic>
class HorsemenTrigger : public Trigger
{
public:
    HorsemenTrigger(PlayerbotAI* botAI) : Trigger(botAI, Name(tactic)) {}
    bool IsActive() override;
};

class AssignCornersAction : public Action
{
public:
    AssignCornersAction(PlayerbotAI* botAI) : Action(botAI, Name(Tactic::Assign)) {}
    bool Execute(Event event) override;
};

class TauntHorsemanAction : public Action
{
public:
    TauntHorsemanAction(PlayerbotAI* botAI) : Action(botAI, Name(Tactic::Taunt)) {}
    bool Execute(Event event) override;
};

template <Tactic tactic>
class HorsemenMoveAction : public MovementAction
{
public:
    HorsemenMoveAction(PlayerbotAI* botAI) : MovementAction(botAI, Name(tactic)) {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

class AttackHorsemanAction : public AttackAction
{
public:
    AttackHorsemanAction(PlayerbotAI* botAI) : AttackAction(botAI, Name(Tactic::Attack)) {}
    bool Execute(Event event) override;
};

class HorsemenFormationMultiplier : public Multiplier
{
public:
    HorsemenFormationMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "naxx40 horsemen formation") {}
    float GetValue(Action* action) override;
};
}  // namespace Naxx40Horsemen

#endif
