/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NAXX40ENCOUNTER_H
#define PLAYERBOTS_NAXX40ENCOUNTER_H

#include "Map.h"
#include "Position.h"

#include <optional>

class Player;

inline constexpr uint32 Naxx40PatchwerkEntry = 351028;
inline constexpr uint32 Naxx40PatchwerkEncounter = 0;
inline constexpr uint32 Naxx40SlimeLiquidId = 21; // Naxxramas slime, which applies spell 28801 on contact.

inline bool IsNaxx40Map(Map const* map)
{
    // Individual Progression uses the otherwise unused heroic slot for the original 40-player raid.
    return map && map->GetId() == 533 && map->GetDifficulty() == RAID_DIFFICULTY_10MAN_HEROIC;
}

std::optional<Position> GetNaxx40Landing(Player const& anchor, Player const& traveler, float x, float y);

#endif
