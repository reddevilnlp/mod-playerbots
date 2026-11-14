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
#include "MoveSplineInitArgs.h"

class Player;

namespace Naxx40GrobbulusClouds
{
    bool OnRoomFloor(Player const& player);
    bool IsEntry(uint32 entry);
    bool HasPendingInjection(Player const& player);
    bool NeedsEscape(Player& player);
    bool ClearsPath(Player& player, Movement::PointsArray const& path);
}

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
