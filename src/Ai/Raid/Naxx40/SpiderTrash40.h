/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SPIDERTRASH40_H
#define PLAYERBOTS_SPIDERTRASH40_H

#include "Action.h"
#include "NamedObjectContext.h"
#include "Strategy.h"
#include "Trigger.h"

#include <optional>

class Player;
class Unit;

inline constexpr char Naxx40SpiderTrashStrategyName[] = "naxx40 spider trash";

std::optional<uint8> Naxx40SpiderTrashRank(Player const& marker, Unit const& enemy);

class Naxx40SpiderActionContext final : public NamedObjectContext<Action>
{
public:
    Naxx40SpiderActionContext();
};

class Naxx40SpiderTriggerContext final : public NamedObjectContext<Trigger>
{
public:
    Naxx40SpiderTriggerContext();
};

class Naxx40SpiderStrategyContext final : public NamedObjectContext<Strategy>
{
public:
    Naxx40SpiderStrategyContext();
};

#endif
