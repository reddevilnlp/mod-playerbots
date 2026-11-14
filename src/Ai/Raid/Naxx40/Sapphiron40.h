/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SAPPHIRON40_H
#define PLAYERBOTS_SAPPHIRON40_H

#include "Action.h"
#include "NamedObjectContext.h"
#include "Trigger.h"

class Multiplier;

class Sapphiron40ActionContext : public NamedObjectContext<Action>
{
public:
    Sapphiron40ActionContext();
};

class Sapphiron40TriggerContext : public NamedObjectContext<Trigger>
{
public:
    Sapphiron40TriggerContext();
};

void AddSapphiron40Triggers(std::vector<TriggerNode*>& triggers);
void AddSapphiron40Multipliers(PlayerbotAI* botAI, std::vector<Multiplier*>& multipliers);

#endif
