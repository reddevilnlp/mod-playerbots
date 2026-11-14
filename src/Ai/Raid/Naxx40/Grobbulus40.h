/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GROBBULUS40_H
#define PLAYERBOTS_GROBBULUS40_H

#include "Action.h"
#include "NamedObjectContext.h"
#include "Strategy.h"
#include "Trigger.h"

class Grobbulus40ActionContext final : public NamedObjectContext<Action>
{
public:
    Grobbulus40ActionContext();
};

class Grobbulus40TriggerContext final : public NamedObjectContext<Trigger>
{
public:
    Grobbulus40TriggerContext();
};

class Grobbulus40StrategyContext final : public NamedObjectContext<Strategy>
{
public:
    Grobbulus40StrategyContext();
};

#endif
