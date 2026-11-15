/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FOURHORSEMEN40_H
#define PLAYERBOTS_FOURHORSEMEN40_H

#include "Action.h"
#include "NamedObjectContext.h"
#include "Trigger.h"

class Multiplier;

class FourHorsemen40ActionContext : public NamedObjectContext<Action>
{
public:
    FourHorsemen40ActionContext();
};

class FourHorsemen40TriggerContext : public NamedObjectContext<Trigger>
{
public:
    FourHorsemen40TriggerContext();
};

void AddFourHorsemen40Triggers(std::vector<TriggerNode*>& triggers);
void AddFourHorsemen40Multipliers(PlayerbotAI* botAI, std::vector<Multiplier*>& multipliers);

#endif
