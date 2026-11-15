/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FourHorsemen40Hazards.h"

#include "Creature.h"
#include "Log.h"
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
    constexpr float ZoneScanRadius = 65.0f;
    constexpr float EscapeRadius = 15.0f;
    constexpr float MovementClearance = 1.5f;
    constexpr float MarkMargin = 3.0f;
    constexpr float MarkEscapeSearch = 60.0f;

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

bool FourHorsemen40Hazards::EscapeMarks(Player& bot, std::span<FourHorsemenMarkSource const> sources)
{
    if (sources.empty())
        return false;

    RefreshVoidZones(bot);
    auto const clearanceAt = [this, sources](float x, float y, float z)
    {
        float gap = Clearance(Position(x, y, z)) - MovementClearance;
        for (FourHorsemenMarkSource const& source : sources)
            gap = std::min(gap, source.location.GetExactDist2d(x, y) - source.radius - MarkMargin);
        return gap;
    };
    if (clearanceAt(bot.GetPositionX(), bot.GetPositionY(), bot.GetPositionZ()) >= 0.0f)
    {
        if (bot.isMoving() && !bot.movespline->Finalized() && !RaidClear::OnItsWayOut(&bot, clearanceAt, 0.0f))
            bot.StopMoving();
        return false;
    }

    // Search before assigning any long return leg. If no single path completely
    // clears the marked casters, the best reachable improvement can be continued.
    if (RaidClear::OnItsWayOut(&bot, clearanceAt, 0.0f))
        return true;
    if (!_markWalk.Go(clearanceAt, 0.0f, 1.0f, 3.0f, MarkEscapeSearch, sources.size() == 1))
        return false;

    if (!bot.movespline->Finalized())
    {
        G3D::Vector3 const end = bot.movespline->FinalDestination();
        LOG_INFO("playerbots", "[Naxx40] Horsemen mark retreat instance={} {} -> ({:.1f}, {:.1f}, {:.2f}) "
            "distance={:.1f}yd clearance={:.1f}yd", bot.GetInstanceId(), bot.GetName(), end.x, end.y, end.z,
            bot.GetExactDist2d(end.x, end.y), clearanceAt(end.x, end.y, end.z));
    }
    return true;
}
