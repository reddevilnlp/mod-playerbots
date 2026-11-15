/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FourHorsemen40Hazards.h"

#include "Creature.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <limits>
#include <list>

namespace
{
    constexpr uint32 NPC_VOID_ZONE = 16697;
    constexpr uint32 SPELL_CONSUMPTION = 28865;
    constexpr uint32 ZoneUpdateMs = 250;
    constexpr float ZoneScanRadius = 45.0f;
    constexpr float EscapeRadius = 15.0f;
    constexpr float MovementClearance = 1.5f;

    float ConsumptionRadius()
    {
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(SPELL_CONSUMPTION);
        float const radius = spell ? spell->Effects[EFFECT_0].CalcRadius() : 0.0f;
        return radius > 0.0f ? radius : 2.0f;
    }
}

void FourHorsemen40Hazards::RefreshVoidZones(Player& bot)
{
    uint32 const now = getMSTime();
    if (_instanceId == bot.GetInstanceId() && _checkedAt && getMSTimeDiff(_checkedAt, now) < ZoneUpdateMs)
        return;

    _instanceId = bot.GetInstanceId();
    _checkedAt = now;
    _voidZones.clear();

    std::list<Creature*> zones;
    bot.GetCreatureListWithEntryInGrid(zones, NPC_VOID_ZONE, ZoneScanRadius);
    for (Creature const* zone : zones)
        if (zone->IsAlive())
            _voidZones.push_back(zone->GetPosition());
}

float FourHorsemen40Hazards::Clearance(Position const& point) const
{
    static float const radius = ConsumptionRadius();
    float gap = std::numeric_limits<float>::max();
    for (Position const& zone : _voidZones)
        gap = std::min(gap, zone.GetExactDist(point) - radius);
    return gap;
}

bool FourHorsemen40Hazards::IsSafeFromVoidZones(Player& bot, Position const& destination)
{
    RefreshVoidZones(bot);
    return Clearance(destination) >= MovementClearance;
}

bool FourHorsemen40Hazards::EscapeVoidZones(Player& bot, Position const& corner, float cornerRadius)
{
    if (IsSafeFromVoidZones(bot, bot.GetPosition()))
        return false;

    // Keep the assigned corner once there. In transit, an escape can also move towards it.
    float const limit = std::max(cornerRadius, bot.GetExactDist2d(&corner));
    auto const clearanceAt = [this, &corner, limit](float x, float y, float z)
    {
        if (corner.GetExactDist2d(x, y) > limit)
            return std::numeric_limits<float>::lowest();
        return Clearance(Position(x, y, z));
    };
    return _walk.Go(clearanceAt, MovementClearance, MovementClearance + 0.5f, 1.0f, EscapeRadius);
}
