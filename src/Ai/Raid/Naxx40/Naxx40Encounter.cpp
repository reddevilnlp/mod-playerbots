/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Naxx40Encounter.h"

#include "Player.h"

#include <array>
#include <cmath>
#include <utility>

std::optional<Position> GetNaxx40Landing(Player const& anchor, Player const& traveler, float x, float y)
{
    return GetNaxx40Landing(anchor, traveler, anchor.GetPosition(), x, y);
}

std::optional<Position> GetNaxx40Landing(Player const& anchor, Player const& traveler, Position const& origin,
                                        float x, float y)
{
    Map* const map = anchor.FindMap();
    if (!IsNaxx40Map(map) || !anchor.IsInWorld() || anchor.IsBeingTeleported() || anchor.IsFalling() ||
        anchor.IsFlying() || anchor.GetTransport() || anchor.GetVehicle())
        return std::nullopt;

    constexpr float FloorProbeLift = 0.6f;
    constexpr float MaximumFloorGap = 0.5f;
    constexpr float EdgeClearance = 0.5f;
    constexpr float LosHeight = 1.0f;
    uint32 const phase = anchor.GetPhaseMask();
    float const collisionHeight = traveler.GetCollisionHeight();
    auto const floorAt = [&](float probeX, float probeY, float referenceZ) -> std::optional<float>
    {
        PositionFullTerrainStatus terrain;
        map->GetFullTerrainStatusForPosition(phase, probeX, probeY, referenceZ + FloorProbeLift, collisionHeight,
                                             terrain);
        if (!std::isfinite(terrain.floorZ) || std::abs(terrain.floorZ - referenceZ) > MaximumFloorGap ||
            (terrain.liquidInfo.Status & MAP_LIQUID_STATUS_IN_CONTACT) || terrain.liquidInfo.Level > terrain.floorZ)
            return std::nullopt;
        return terrain.floorZ;
    };

    auto const anchorFloor = floorAt(origin.GetPositionX(), origin.GetPositionY(), origin.GetPositionZ());
    if (!anchorFloor)
        return std::nullopt;

    auto const landingFloor = floorAt(x, y, *anchorFloor);
    if (!landingFloor || !map->isInLineOfSight(origin.GetPositionX(), origin.GetPositionY(), *anchorFloor + LosHeight,
                                             x, y, *landingFloor + LosHeight, phase, LINEOFSIGHT_ALL_CHECKS,
                                             VMAP::ModelIgnoreFlags::Nothing))
        return std::nullopt;

    // A floor hit at the center can still be on the lip of a channel or ledge.
    constexpr std::array offsets = {std::pair{EdgeClearance, 0.0f}, std::pair{-EdgeClearance, 0.0f},
                                    std::pair{0.0f, EdgeClearance}, std::pair{0.0f, -EdgeClearance}};
    for (auto const& [dx, dy] : offsets)
        if (!floorAt(x + dx, y + dy, *landingFloor))
            return std::nullopt;

    return Position(x, y, *landingFloor, traveler.GetOrientation());
}
