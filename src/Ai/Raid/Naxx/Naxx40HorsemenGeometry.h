/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NAXX40_HORSEMEN_GEOMETRY_H
#define PLAYERBOTS_NAXX40_HORSEMEN_GEOMETRY_H

#include <array>
#include <optional>
#include <vector>

#include "Position.h"

class Player;

namespace Naxx40Horsemen
{
enum class Corner : uint8
{
    Korthazz,
    Mograine,
    Blaumeux,
    Zeliek,
    None
};

inline constexpr std::size_t CORNER_COUNT = 4;
inline constexpr uint8 ZELIEK_SUPPORT_SLOTS = 8;

struct MarkSource
{
    Position position;
    float radius = 0.0f;
};

using MarkSources = std::array<MarkSource, CORNER_COUNT>;

Position const& CornerPosition(Corner corner);
float MarkRadius(Corner corner);
Position WaitPosition(Player* player, MarkSources const& sources, std::vector<Position> const& voidZones,
                      uint8 marksToClear, uint8 slot);
Position SupportPosition(Corner corner, uint8 slot);
bool IsNearVoidZone(Position const& position, std::vector<Position> const& voidZones);
std::optional<Position> VoidZoneEscape(Player* player, std::vector<Position> const& voidZones);
std::optional<Position> RouteAroundVoidZones(Player* player, Position const& destination,
                                             std::vector<Position> const& voidZones);
}  // namespace Naxx40Horsemen

#endif
