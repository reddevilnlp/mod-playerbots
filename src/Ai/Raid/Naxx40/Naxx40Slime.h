/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NAXX40SLIME_H
#define PLAYERBOTS_NAXX40SLIME_H

#include "Action.h"
#include "NamedObjectContext.h"
#include "Strategy.h"
#include "Trigger.h"

inline constexpr char Naxx40SlimeStrategyName[] = "naxx40 slime";

void AuthorizeNaxx40SlimeCrossing(PlayerbotAI& ai);
bool IsNaxx40SlimeCrossingAuthorized(PlayerbotAI& ai);
bool IsNaxx40SlimeEscapeActive(PlayerbotAI& ai);

class Naxx40SlimeActionContext final : public NamedObjectContext<Action>
{
public:
    Naxx40SlimeActionContext();
};

class Naxx40SlimeTriggerContext final : public NamedObjectContext<Trigger>
{
public:
    Naxx40SlimeTriggerContext();
};

class Naxx40SlimeStrategyContext final : public NamedObjectContext<Strategy>
{
public:
    Naxx40SlimeStrategyContext();
};

class Naxx40SlimeValueContext final : public NamedObjectContext<UntypedValue>
{
public:
    Naxx40SlimeValueContext();
};

#endif
