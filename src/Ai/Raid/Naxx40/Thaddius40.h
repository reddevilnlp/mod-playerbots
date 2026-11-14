/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_THADDIUS40_H
#define PLAYERBOTS_THADDIUS40_H

#include "Action.h"
#include "NamedObjectContext.h"
#include "Trigger.h"

class Multiplier;

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
