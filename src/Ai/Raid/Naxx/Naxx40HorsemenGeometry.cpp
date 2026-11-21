/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Naxx40HorsemenGeometry.h"

#include <algorithm>
#include <cmath>
#include <compare>
#include <limits>
#include <numbers>

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
std::array<Position, 11> const waitPositions = {{
    {2528.2f, -2950.3f, 244.87f},
    {2529.5f, -2953.0f, 244.78f},
    {2526.0f, -2947.0f, 245.60f},
    {2532.0f, -2948.0f, 245.69f},
    {2525.0f, -2952.0f, 245.11f},
    {2523.4f, -2950.5f, 245.82f},
    {2537.0f, -2943.0f, 241.57f},
    {2517.0f, -2954.0f, 245.66f},
    {2521.0f, -2947.0f, 245.41f},
    {2528.0f, -2955.0f, 245.55f},
    {2536.0f, -2956.0f, 243.12f},
}};

// Dry positions on both sides of the platform. They cover corners vacated by a moving Horseman.
std::array<Position, 17> const lateralWaitPositions = {{
    {2492.5f, -2971.5f, 241.28f},
    {2495.0f, -2919.0f, 241.28f},
    {2566.0f, -2990.0f, 241.41f},
    {2551.0f, -2975.0f, 241.34f},
    {2575.0f, -2964.0f, 241.32f},
    {2515.0f, -2904.0f, 241.28f},
    {2540.0f, -2999.0f, 241.32f},
    {2480.0f, -2939.0f, 241.28f},
    {2519.0f, -2995.0f, 241.29f},
    {2484.0f, -2978.0f, 241.28f},
    {2561.0f, -2947.0f, 241.28f},
    {2512.0f, -2977.0f, 241.28f},
    {2551.0f, -2900.0f, 241.28f},
    {2483.0f, -2982.0f, 241.43f},
    {2478.0f, -2977.0f, 241.28f},
    {2460.0f, -2958.0f, 241.28f},
    {2607.0f, -2958.0f, 241.39f},
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
constexpr float WITHDRAWAL_DISTANCE = 65.0f;
constexpr float MARK_RADIUS_MARGIN = 4.0f;
constexpr uint8 MAX_CENTRAL_ROUTE_CHECKS = 3;
constexpr uint8 MAX_LATERAL_ROUTE_CHECKS = 4;

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

bool EntersAndStaysInRoom(Movement::PointsArray const& path)
{
    auto inside = [](auto const& node) { return IsInsideRoom({node.x, node.y, node.z}); };
    auto const entry = std::find_if(path.begin(), path.end(), inside);
    return entry != path.end() && std::all_of(entry, path.end(), inside);
}

struct WithdrawalClearance
{
    bool clearsAllAt65;
    bool clearsPendingMarks;
    bool clearsEveryMark;
    float nearestSource;

    auto operator<=>(WithdrawalClearance const&) const = default;
};

WithdrawalClearance GetWithdrawalClearance(Position const& position, MarkSources const& sources, uint8 marksToClear)
{
    float nearest = WITHDRAWAL_DISTANCE;
    bool clearsPendingMarks = true;
    bool clearsEveryMark = true;
    for (std::size_t index = 0; index < sources.size(); ++index)
    {
        auto const& source = sources[index];
        if (source.radius <= 0.0f)
            continue;
        float const distance = position.GetExactDist2d(source.position);
        nearest = std::min(nearest, distance);
        if (distance < source.radius + MARK_RADIUS_MARGIN)
        {
            clearsEveryMark = false;
            if (marksToClear & (1u << index))
                clearsPendingMarks = false;
        }
    }

    return {nearest >= WITHDRAWAL_DISTANCE, clearsPendingMarks, clearsEveryMark, nearest};
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
    WithdrawalClearance const currentClearance = GetWithdrawalClearance(current, sources, marksToClear);
    if (currentClearance.clearsAllAt65 || IsNearVoidZone(current, voidZones))
        return current;

    struct Candidate
    {
        Position const* position;
        WithdrawalClearance clearance;
        float distanceSquared;
        bool lateral;
    };

    // A spread-out room can leave no 65-yard point. Rank fallback points by mark coverage and clearance;
    // reaching one does not mean the bot is outside every mark or can end its withdrawal.
    std::array<Candidate, waitPositions.size() + lateralWaitPositions.size() + CORNER_COUNT> candidates;
    auto end = candidates.begin();
    auto addCandidates = [&](auto const& points, bool lateral)
    {
        for (std::size_t index = 0; index < points.size(); ++index)
        {
            Position const& point = points[(index + slot) % points.size()];
            WithdrawalClearance const clearance = GetWithdrawalClearance(point, sources, marksToClear);
            if (clearance > currentClearance && !IsNearVoidZone(point, voidZones))
                *end++ = {&point, clearance, current.GetExactDist2dSq(point), lateral};
        }
    };
    addCandidates(waitPositions, false);
    addCandidates(lateralWaitPositions, true);
    addCandidates(cornerPositions, true);
    std::sort(candidates.begin(), end,
              [](Candidate const& left, Candidate const& right)
              {
                  return left.clearance == right.clearance ? left.distanceSquared < right.distanceSquared
                                                           : left.clearance > right.clearance;
              });

    std::optional<Position> blocked;
    WithdrawalClearance blockedClearance = currentClearance;
    Position best = current;
    WithdrawalClearance bestClearance = currentClearance;
    uint8 centralChecks = 0;
    uint8 lateralChecks = 0;
    for (auto candidate = candidates.begin(); candidate != end; ++candidate)
    {
        // Several central positions share the same ramp. Reserve path checks for both sides of the room.
        uint8& checks = candidate->lateral ? lateralChecks : centralChecks;
        uint8 const limit = candidate->lateral ? MAX_LATERAL_ROUTE_CHECKS : MAX_CENTRAL_ROUTE_CHECKS;
        if (checks >= limit)
            continue;
        ++checks;

        Position point = *candidate->position;
        float const x = point.GetPositionX();
        float const y = point.GetPositionY();
        float const z = point.GetPositionZ();
        float const ground = player->GetMapHeight(x, y, z + heightTolerance, true, heightTolerance * 2.0f);
        point.Relocate(x, y, ground);
        if (std::abs(ground - z) > heightTolerance || !IsInsideRoom(point) || IsNearVoidZone(point, voidZones))
            continue;

        PathGenerator path(player);
        if (!BuildRoute(player, path, point) || !EntersAndStaysInRoom(path.GetPath()))
            continue;
        auto const& endpoint = path.GetPath().back();
        if (point.GetExactDist(endpoint.x, endpoint.y, endpoint.z) > heightTolerance)
            continue;
        WithdrawalClearance const clearance =
            GetWithdrawalClearance({endpoint.x, endpoint.y, endpoint.z}, sources, marksToClear);
        if (clearance <= bestClearance)
            continue;
        if (FindRouteBlock(current, path.GetPath(), voidZones))
        {
            if (clearance > blockedClearance)
            {
                blocked = point;
                blockedClearance = clearance;
            }
            continue;
        }

        best = point;
        bestClearance = clearance;
        if (clearance >= candidate->clearance)
            break;
    }

    if (blocked && blockedClearance > bestClearance)
        if (auto waypoint = RouteAroundVoidZones(player, *blocked, voidZones))
            return *waypoint;

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
        return EntersAndStaysInRoom(path.GetPath());
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
