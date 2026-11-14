/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PATCHWERK40_H
#define PLAYERBOTS_PATCHWERK40_H

#include "Define.h"
#include "NamedObjectContext.h"
#include "Position.h"

#include <vector>

class Action;
class Multiplier;
class Player;
class PlayerbotAI;
class Trigger;
class TriggerNode;
class UntypedValue;

void AddPatchwerk40Triggers(std::vector<TriggerNode*>& triggers);
void AddPatchwerk40Multipliers(PlayerbotAI* ai, std::vector<Multiplier*>& multipliers);

namespace Patchwerk40Position
{
inline constexpr float PullCampX = 3103.59f;
inline constexpr float PullCampY = -3106.17f;
inline constexpr float PullCampZ = 294.07f;
inline Position const PullCamp(PullCampX, PullCampY, PullCampZ);
inline Position const RaidCamp(3103.6f, -3091.2f, 294.0766f);
}

namespace Patchwerk40TankSupplies
{
constexpr uint32 FlaskItem = 13510;
constexpr uint32 FlaskAura = 17626;
constexpr uint32 FoodItem = 21023;
constexpr uint32 FoodAura = 25661;
constexpr uint32 EatingAura = 25660;
constexpr uint32 StoneshieldItem = 13455;
constexpr uint32 StoneshieldAura = 17540;
constexpr int32 MinimumFlaskMs = 180000;
constexpr int32 MinimumFoodMs = 180000;
constexpr int32 MinimumStoneshieldMs = 90000;

bool HasTimeRemaining(Player const& player, uint32 auraId, int32 minimumMs);
}

enum class Patchwerk40TankPreparation
{
    Idle,
    Camp,
    Shot
};

void SetPatchwerk40TankPreparation(PlayerbotAI& ai, Patchwerk40TankPreparation preparation);

class Patchwerk40ActionContext final : public NamedObjectContext<Action>
{
public:
    Patchwerk40ActionContext();
};

class Patchwerk40TriggerContext final : public NamedObjectContext<Trigger>
{
public:
    Patchwerk40TriggerContext();
};

class Patchwerk40ValueContext final : public NamedObjectContext<UntypedValue>
{
public:
    Patchwerk40ValueContext();
};

#endif
