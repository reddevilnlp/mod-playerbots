/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FOURHORSEMEN40HAZARDS_H
#define PLAYERBOTS_FOURHORSEMEN40HAZARDS_H

#include "Ai/Raid/RaidClear/Common/WayOut.h"

#include <span>
#include <vector>

struct FourHorsemenMarkSource
{
    Position location;
    float radius;
};

class FourHorsemen40Hazards
{
public:
    explicit FourHorsemen40Hazards(PlayerbotAI* ai) : _ai(ai) {}

    bool EscapeVoidZones(Player& bot, Position const& corner, float cornerRadius);
    bool IsSafeFromVoidZones(Player& bot, Position const& destination);
    bool WalkRear(Player& bot, Position const& destination, float arrival, bool detour = true);
    bool ApproachRear(Player& bot, Unit const& target, float range, float arrival);
    bool EscapeMarks(Player& bot, std::span<FourHorsemenMarkSource const> sources);

private:
    void RefreshVoidZones(Player& bot);
    float Clearance(Position const& point) const;
    Position const* FirstBlockedZone(Player const& bot, std::span<G3D::Vector3 const> path) const;
    bool SafeSpline(Player const& bot, uint32 splineId) const;
    bool Launch(Player& bot, Movement::PointsArray const& path, uint32& splineId);

    PlayerbotAI* _ai;
    std::vector<Position> _voidZones;
    Position _blockedGoal;
    uint32 _instanceId = 0;
    uint32 _checkedAt = 0;
    uint32 _rearSpline = 0;
    uint32 _rearRetryAt = 0;
    uint32 _voidRetryAt = 0;
    uint32 _markSpline = 0;
    uint32 _markRetryAt = 0;
};

#endif
