/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_THADDIUS40_H
#define PLAYERBOTS_THADDIUS40_H

#include "Action.h"
#include "Define.h"
#include "NamedObjectContext.h"
#include "Trigger.h"

class Multiplier;
class Player;

namespace Thaddius40
{
// Individual Progression registers these entries in the Naxxramas instance script.
inline constexpr uint32 Encounter = 9;
inline constexpr uint32 StalaggData = 101;
inline constexpr uint32 FeugenData = 102;
inline constexpr uint32 ThaddiusData = 103;
inline constexpr uint32 StalaggEntry = 351001;
inline constexpr uint32 FeugenEntry = 351002;
inline constexpr uint32 ThaddiusEntry = 351000;

struct OpeningAssignment
{
    uint8 side; // 0: Stalagg, 1: Feugen
    bool tank;
};

OpeningAssignment AssignOpening(Player* player);
}

class Thaddius40ActionContext : public NamedObjectContext<Action>
{
public:
    Thaddius40ActionContext();
};

class Thaddius40TriggerContext : public NamedObjectContext<Trigger>
{
public:
    Thaddius40TriggerContext();
};

void AddThaddius40Triggers(std::vector<TriggerNode*>& triggers);
void AddThaddius40Multipliers(PlayerbotAI* botAI, std::vector<Multiplier*>& multipliers);

#endif
