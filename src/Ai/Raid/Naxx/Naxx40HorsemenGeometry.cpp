/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Naxx40HorsemenGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <tuple>

#include "EncounterHelpers.h"
#include "Map.h"
#include "PathGenerator.h"
#include "Player.h"

namespace Naxx40Horsemen
{
namespace
{
std::array<Position, CORNER_COUNT> const cornerPositions = {{
    {2534.5f, -3014.7f, 241.33f},
    {2591.5f, -2964.0f, 241.34f},
    {2469.4f, -2947.6f, 241.28f},
    {2517.8f, -2896.6f, 241.28f},
}};
constexpr std::array<float, CORNER_COUNT> MARK_RADII = {55.0f, 55.0f, 45.0f, 45.0f};

// The platform and the foot of its ramps are reachable by normal movement pathfinding.
std::array<Position, 9> const waitPositions = {{
    {2528.2f, -2950.3f, 244.87f},
    {2529.5f, -2953.0f, 244.78f},
    {2526.0f, -2947.0f, 245.60f},
    {2532.0f, -2948.0f, 245.69f},
    {2525.0f, -2952.0f, 245.11f},
    {2523.4f, -2950.5f, 245.82f},
    {2537.0f, -2943.0f, 241.57f},
    {2517.0f, -2954.0f, 245.66f},
    {2521.0f, -2947.0f, 245.41f},
}};

std::array<std::array<Position, 2>, 3> const supportPositions = {{
    {{{2534.0f, -3008.0f, 241.42f}, {2538.0f, -3008.0f, 241.42f}}},
    {{{2576.0f, -2966.0f, 241.42f}, {2582.0f, -2972.0f, 241.42f}}},
    {{{2483.0f, -2958.0f, 241.42f}, {2488.0f, -2951.0f, 241.42f}}},
}};

// The core adds unit combat reach to Holy Wrath's jump distance. Keep the closest healer slots first.
std::array<Position, ZELIEK_SUPPORT_SLOTS> const zeliekSupportPositions = {{
    {2526.0f, -2914.0f, 241.42f},
    {2501.0f, -2910.0f, 241.42f},
    {2533.0f, -2897.0f, 241.42f},
    {2503.0f, -2893.0f, 242.46f},
    {2512.0f, -2923.0f, 241.42f},
    {2539.0f, -2923.0f, 241.42f},
    {2526.0f, -2929.0f, 241.65f},
    {2549.0f, -2910.0f, 242.74f},
}};

// Consumption has a 2 yard radius. Include player reach and room for movement to complete.
constexpr float VOID_ZONE_CLEARANCE = 4.5f;
constexpr float VOID_ZONE_VERTICAL_RANGE = 3.0f;
constexpr float VOID_ZONE_ESCAPE_STEP = 7.0f;
constexpr uint8 VOID_ZONE_ESCAPE_DIRECTIONS = 12;
constexpr float VOID_ZONE_DETOUR_DISTANCE = 8.0f;
constexpr float MIN_DETOUR_STEP = 4.0f;
constexpr float MARK_CLEARANCE = 4.0f;

bool IsInsideRoom(Position const& position)
{
    constexpr float roomCenterX = 2530.0f;
    constexpr float roomCenterY = -2954.0f;
    constexpr float gateY = -3016.0f;
    float const offsetX = position.GetPositionX() - roomCenterX;
    float const offsetY = position.GetPositionY() - roomCenterY;
    float const diagonal = offsetX - offsetY;
    return position.GetPositionY() >= gateY && std::abs(offsetX + offsetY) <= 75.0f && diagonal >= -92.0f &&
           diagonal <= 82.0f && position.GetPositionZ() >= 240.0f && position.GetPositionZ() <= 247.0f;
}

bool IsOnZoneLevel(Position const& position, Position const& zone)
{
    return std::abs(position.GetPositionZ() - zone.GetPositionZ()) <= VOID_ZONE_VERTICAL_RANGE;
}

float NearestVoidZoneDistanceSq(Position const& position, std::vector<Position> const& voidZones)
{
    float distance = std::numeric_limits<float>::max();
    for (Position const& zone : voidZones)
        if (IsOnZoneLevel(position, zone))
            distance = std::min(distance, position.GetExactDist2dSq(zone));
    return distance;
}

std::optional<float> VoidZoneEntry(Position const& from, Position const& to, Position const& zone)
{
    constexpr float radiusSq = VOID_ZONE_CLEARANCE * VOID_ZONE_CLEARANCE;
    float first = 0.0f;
    float last = 1.0f;
    // Constrain the segment to the hazard's floor before testing the horizontal circle.
    float const deltaZ = to.GetPositionZ() - from.GetPositionZ();
    float const offsetZ = zone.GetPositionZ() - from.GetPositionZ();
    if (std::abs(deltaZ) > 0.01f)
    {
        float const lower = (offsetZ - VOID_ZONE_VERTICAL_RANGE) / deltaZ;
        float const upper = (offsetZ + VOID_ZONE_VERTICAL_RANGE) / deltaZ;
        first = std::max(first, std::min(lower, upper));
        last = std::min(last, std::max(lower, upper));
    }
    else if (!IsOnZoneLevel(from, zone))
        return std::nullopt;

    float const deltaX = to.GetPositionX() - from.GetPositionX();
    float const deltaY = to.GetPositionY() - from.GetPositionY();
    float const lengthSq = deltaX * deltaX + deltaY * deltaY;
    if (lengthSq > 0.01f)
    {
        float const zoneX = zone.GetPositionX() - from.GetPositionX();
        float const zoneY = zone.GetPositionY() - from.GetPositionY();
        float const projection = (deltaX * zoneX + deltaY * zoneY) / lengthSq;
        float const gapSq =
            zone.GetExactDist2dSq(from.GetPositionX() + projection * deltaX, from.GetPositionY() + projection * deltaY);
        if (gapSq >= radiusSq)
            return std::nullopt;
        float const halfWidth = std::sqrt((radiusSq - gapSq) / lengthSq);
        first = std::max(first, projection - halfWidth);
        last = std::min(last, projection + halfWidth);
    }
    else if (from.GetExactDist2dSq(zone) >= radiusSq)
        return std::nullopt;

    return first <= last ? std::optional{first} : std::nullopt;
}

Position const* FirstVoidZoneOnSegment(Position const& from, Position const& to, std::vector<Position> const& voidZones,
                                       bool escaping = false)
{
    float earliest = std::numeric_limits<float>::max();
    Position const* blocker = nullptr;
    for (Position const& zone : voidZones)
    {
        if (escaping && IsOnZoneLevel(from, zone) &&
            from.GetExactDist2dSq(zone) < VOID_ZONE_CLEARANCE * VOID_ZONE_CLEARANCE)
            continue;

        if (auto entry = VoidZoneEntry(from, to, zone); entry && *entry < earliest)
        {
            earliest = *entry;
            blocker = &zone;
        }
    }

    return blocker;
}

struct RouteBlock
{
    Position const* zone;
    Position from;
    Position to;
};

std::optional<RouteBlock> FindRouteBlock(Position const& start, Movement::PointsArray const& path,
                                         std::vector<Position> const& voidZones)
{
    Position previous = start;
    for (auto const& point : path)
    {
        Position const next(point.x, point.y, point.z);
        if (Position const* zone = FirstVoidZoneOnSegment(previous, next, voidZones))
            return RouteBlock{zone, previous, next};
        previous = next;
    }
    return std::nullopt;
}

bool BuildRoute(Player* player, PathGenerator& path, Position const& destination)
{
    float x = destination.GetPositionX();
    float y = destination.GetPositionY();
    float z = destination.GetPositionZ();
    if (!path.CalculatePath(x, y, z) || !(path.GetPathType() & PATHFIND_NORMAL) || path.GetPath().empty())
        return false;

    if (!(path.GetPathType() & PATHFIND_NOT_USING_PATH))
        return true;

    // The core's straight path fallback can cross closed doors or walls.
    constexpr float endpointTolerance = 0.5f;
    return player->GetMap()->CheckCollisionAndGetValidCoords(player, player->GetPositionX(), player->GetPositionY(),
                                                             player->GetPositionZ(), x, y, z) &&
           destination.GetExactDist(x, y, z) <= endpointTolerance;
}

std::tuple<float, float> MarkClearance(Position const& position, MarkSources const& sources, uint8 marksToClear)
{
    // Stop ranking gaps once they are large enough so different slots can use different waiting points.
    float needed = MARK_CLEARANCE;
    float all = MARK_CLEARANCE;
    for (std::size_t index = 0; index < sources.size(); ++index)
    {
        auto const& source = sources[index];
        if (source.radius <= 0.0f)
            continue;
        float const clearance = position.GetExactDist2d(source.position) - source.radius;
        if (marksToClear & (1u << index))
            needed = std::min(needed, clearance);
        all = std::min(all, clearance);
    }

    return {needed, all};
}
}  // namespace

Position const& CornerPosition(Corner corner)
{
    std::size_t const index = static_cast<std::size_t>(corner);
    return index < cornerPositions.size() ? cornerPositions[index] : waitPositions.front();
}

float MarkRadius(Corner corner)
{
    std::size_t const index = static_cast<std::size_t>(corner);
    return index < MARK_RADII.size() ? MARK_RADII[index] : 0.0f;
}

Position WaitPosition(Player* player, MarkSources const& sources, std::vector<Position> const& voidZones,
                      uint8 marksToClear, uint8 slot)
{
    constexpr float heightTolerance = 1.5f;
    Position const current = player->GetPosition();
    Position best = current;
    std::tuple<float, float> bestClearance = {std::numeric_limits<float>::lowest(),
                                              std::numeric_limits<float>::lowest()};
    // If displacement made the centre unsafe, do not move from somewhere that already clears the marks.
    if (IsInsideRoom(current) && !IsNearVoidZone(current, voidZones))
        bestClearance = MarkClearance(current, sources, marksToClear);

    for (std::size_t index = 0; index < waitPositions.size(); ++index)
    {
        Position candidate = waitPositions[(slot + index) % waitPositions.size()];
        auto const clearance = MarkClearance(candidate, sources, marksToClear);
        if (clearance <= bestClearance)
            continue;

        float const x = candidate.GetPositionX();
        float const y = candidate.GetPositionY();
        float const z = candidate.GetPositionZ();
        float const ground = player->GetMapHeight(x, y, z + heightTolerance, true, heightTolerance * 2.0f);
        candidate.Relocate(x, y, ground);
        if (std::abs(ground - z) > heightTolerance || !IsInsideRoom(candidate) || IsNearVoidZone(candidate, voidZones))
            continue;

        best = candidate;
        bestClearance = clearance;
    }

    return best;
}

Position SupportPosition(Corner corner, uint8 slot)
{
    if (corner == Corner::Zeliek)
        return zeliekSupportPositions[slot % zeliekSupportPositions.size()];

    std::size_t const index = static_cast<std::size_t>(corner);
    if (index >= supportPositions.size())
        return CornerPosition(corner);

    return supportPositions[index][slot % supportPositions[index].size()];
}

bool IsNearVoidZone(Position const& position, std::vector<Position> const& voidZones)
{
    return NearestVoidZoneDistanceSq(position, voidZones) < VOID_ZONE_CLEARANCE * VOID_ZONE_CLEARANCE;
}

std::optional<Position> VoidZoneEscape(Player* player, std::vector<Position> const& voidZones)
{
    Position const current = player->GetPosition();
    float bestDistanceSq = NearestVoidZoneDistanceSq(current, voidZones);
    std::optional<Position> best;
    constexpr float angleStep = 2.0f * std::numbers::pi_v<float> / VOID_ZONE_ESCAPE_DIRECTIONS;
    for (uint8 index = 0; index < VOID_ZONE_ESCAPE_DIRECTIONS; ++index)
    {
        float const angle = index * angleStep;
        float const x = current.GetPositionX() + std::cos(angle) * VOID_ZONE_ESCAPE_STEP;
        float const y = current.GetPositionY() + std::sin(angle) * VOID_ZONE_ESCAPE_STEP;
        Position candidate(x, y, current.GetPositionZ());
        if (!IsInsideRoom(candidate) || NearestVoidZoneDistanceSq(candidate, voidZones) <= bestDistanceSq)
            continue;

        float stepX, stepY, stepZ;
        if (!EncounterHelpers::CanTakeStepTowards(player, x, y, VOID_ZONE_ESCAPE_STEP, stepX, stepY, stepZ))
            continue;

        candidate.Relocate(stepX, stepY, stepZ);
        float const distanceSq = NearestVoidZoneDistanceSq(candidate, voidZones);
        if (distanceSq <= bestDistanceSq || !IsInsideRoom(candidate) ||
            FirstVoidZoneOnSegment(current, candidate, voidZones, true))
            continue;

        best = candidate;
        bestDistanceSq = distanceSq;
    }

    return best;
}

std::optional<Position> RouteAroundVoidZones(Player* player, Position const& destination,
                                             std::vector<Position> const& voidZones)
{
    Position const current = player->GetPosition();
    if (IsNearVoidZone(current, voidZones) || IsNearVoidZone(destination, voidZones))
        return std::nullopt;

    PathGenerator normalPath(player);
    if (!BuildRoute(player, normalPath, destination))
        return std::nullopt;
    if (voidZones.empty())
        return destination;

    std::optional<RouteBlock> const block = FindRouteBlock(current, normalPath.GetPath(), voidZones);
    if (!block)
        return destination;

    // Approach a distant obstacle on the verified route before searching locally for a detour.
    auto canReach = [&](Position const& point)
    {
        if (!IsInsideRoom(point) || current.GetExactDist2d(point) < MIN_DETOUR_STEP || IsNearVoidZone(point, voidZones))
            return false;
        PathGenerator path(player);
        if (!BuildRoute(player, path, point) || FindRouteBlock(current, path.GetPath(), voidZones))
            return false;
        return std::all_of(path.GetPath().begin(), path.GetPath().end(),
                           [](auto const& node) { return IsInsideRoom({node.x, node.y, node.z}); });
    };

    if (current.GetExactDist2d(block->from) >= 2.0f * VOID_ZONE_DETOUR_DISTANCE && canReach(block->from))
        return block->from;

    float const heading = std::atan2(block->to.GetPositionY() - block->from.GetPositionY(),
                                     block->to.GetPositionX() - block->from.GetPositionX());
    constexpr float radiansPerStep = std::numbers::pi_v<float> / 6.0f;
    constexpr std::array<int, 6> detourOffsets = {2, -2, 3, -3, 4, -4};
    for (int offset : detourOffsets)
    {
        float const angle = heading + offset * radiansPerStep;
        float const x = block->zone->GetPositionX() + std::cos(angle) * VOID_ZONE_DETOUR_DISTANCE;
        float const y = block->zone->GetPositionY() + std::sin(angle) * VOID_ZONE_DETOUR_DISTANCE;
        float const z = player->GetMapHeight(x, y, block->from.GetPositionZ() + VOID_ZONE_VERTICAL_RANGE, true,
                                             2.0f * VOID_ZONE_VERTICAL_RANGE);
        Position const point(x, y, z);
        if (canReach(point))
            return point;
    }

    return std::nullopt;
}
}  // namespace Naxx40Horsemen
