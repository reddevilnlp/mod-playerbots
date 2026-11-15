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
    explicit FourHorsemen40Hazards(PlayerbotAI* ai) : _ai(ai), _walk(ai) {}

    bool EscapeVoidZones(Player& bot, Position const& corner, float cornerRadius);
    bool IsSafeFromVoidZones(Player& bot, Position const& destination);
    bool EscapeMarks(Player& bot, std::span<FourHorsemenMarkSource const> sources);

private:
    void RefreshVoidZones(Player& bot);
    float Clearance(Position const& point) const;

    PlayerbotAI* _ai;
    RaidClear::WayOutWalk _walk;
    std::vector<Position> _voidZones;
    uint32 _instanceId = 0;
    uint32 _checkedAt = 0;
    uint32 _markSpline = 0;
    uint32 _markRetryAt = 0;
};

#endif
