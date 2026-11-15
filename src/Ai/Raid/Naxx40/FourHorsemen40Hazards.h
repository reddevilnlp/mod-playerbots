/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_FOURHORSEMEN40HAZARDS_H
#define PLAYERBOTS_FOURHORSEMEN40HAZARDS_H

#include "Ai/Raid/RaidClear/Common/WayOut.h"

#include <vector>

class FourHorsemen40Hazards
{
public:
    explicit FourHorsemen40Hazards(PlayerbotAI* ai) : _walk(ai) {}

    bool EscapeVoidZones(Player& bot, Position const& corner, float cornerRadius);
    bool IsSafeFromVoidZones(Player& bot, Position const& destination);

private:
    void RefreshVoidZones(Player& bot);
    float Clearance(Position const& point) const;

    RaidClear::WayOutWalk _walk;
    std::vector<Position> _voidZones;
    uint32 _instanceId = 0;
    uint32 _checkedAt = 0;
};

#endif
