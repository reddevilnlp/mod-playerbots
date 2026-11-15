/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MILITARYTRASH40_H
#define PLAYERBOTS_MILITARYTRASH40_H

#include "Action.h"
#include "NamedObjectContext.h"
#include "Strategy.h"
#include "Trigger.h"

class Player;
class Unit;

inline constexpr char Naxx40MilitaryTrashStrategyName[] = "naxx40 military trash";
inline constexpr uint32 Naxx40DeathknightCaptain = 351048;
inline constexpr uint32 Naxx40ShadeOfNaxxramas = 351056;
inline constexpr uint32 Naxx40NecropolisAcolyte = 351070;

bool IsNaxx40MilitaryTrash(Unit const& unit);
bool MayPetAttackNaxx40MilitaryTrash(Player const& owner, Unit const& target);
bool CanEngageNaxx40NecropolisAcolyte(Player const& bot, Unit const& target);

class Naxx40MilitaryActionContext final : public NamedObjectContext<Action>
{
public:
    Naxx40MilitaryActionContext();
};

class Naxx40MilitaryTriggerContext final : public NamedObjectContext<Trigger>
{
public:
    Naxx40MilitaryTriggerContext();
};

class Naxx40MilitaryStrategyContext final : public NamedObjectContext<Strategy>
{
public:
    Naxx40MilitaryStrategyContext();
};

#endif
