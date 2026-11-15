/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FourHorsemen40Hazards.h"

#include "Creature.h"
#include "Group.h"
#include "Log.h"
#include "MotionMaster.h"
#include "MoveSplineInit.h"
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
    constexpr float MarkEscapeSearch = 39.0f;
    constexpr float MarkEscapeMaxPath = 45.0f;
    constexpr float MarkRetreatSpacing = 12.0f;

    bool InsideHorsemenRoom(float x, float y, float z)
    {
        // The entrance gate is at Y -3017.17. This boundary preserves the
        // dry Korth'azz pads while excluding the corridor from retreat paths.
        return x >= 2450.0f && x <= 2630.0f && y >= -3016.0f && y <= -2880.0f &&
            z >= 239.0f && z <= 248.0f;
    }

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
    if (sources.empty() || !_ai->CanMove())
        return false;

    RefreshVoidZones(bot);
    auto const markClearanceAt = [this, sources](float x, float y, float z)
    {
        if (!InsideHorsemenRoom(x, y, z))
            return std::numeric_limits<float>::lowest();
        float gap = Clearance(Position(x, y, z)) - MovementClearance;
        for (FourHorsemenMarkSource const& source : sources)
            gap = std::min(gap, source.location.GetExactDist2d(x, y) - source.radius - MarkMargin);
        return gap;
    };
    auto const roomPath = [](auto const& path)
    {
        if (path.empty())
            return false;
        float length = 0.0f;
        for (std::size_t i = 0; i < path.size(); ++i)
        {
            auto const& point = path[i];
            if (!InsideHorsemenRoom(point.x, point.y, point.z))
                return false;
            if (i)
                length += (point - path[i - 1]).length();
        }
        return length <= MarkEscapeMaxPath;
    };

    float const current = markClearanceAt(bot.GetPositionX(), bot.GetPositionY(), bot.GetPositionZ());
    if (bot.isMoving() && !bot.movespline->Finalized())
    {
        G3D::Vector3 const end = bot.movespline->FinalDestination();
        float const endClearance = markClearanceAt(end.x, end.y, end.z);
        if (_markSpline == bot.movespline->GetId() && (endClearance >= 0.0f ||
            (current < 0.0f && endClearance > current)))
            return current < 0.0f;

        // A normal chase may have planned a detour through the corridor.
        auto const& spline = bot.movespline->_Spline();
        bool outsideRoom = false;
        for (int32 i = bot.movespline->_currentSplineIdx() + 1; i <= spline.last(); ++i)
        {
            G3D::Vector3 const& point = spline.getPoint(i);
            if (!InsideHorsemenRoom(point.x, point.y, point.z))
            {
                outsideRoom = true;
                break;
            }
        }
        if (outsideRoom || current >= 0.0f)
            bot.StopMoving();
    }
    if (current >= 0.0f || !InsideHorsemenRoom(bot.GetPositionX(), bot.GetPositionY(), bot.GetPositionZ()))
        return false;
    uint32 const now = getMSTime();
    if (_markRetryAt && getMSTimeDiff(_markRetryAt, now) < RaidClear::WAY_OUT_RETRY_MS)
        return false;
    _markRetryAt = now;

    std::vector<Position> occupied;
    if (Group* group = bot.GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource(); member && member != &bot && member->IsInWorld() &&
                member->IsAlive() && member->IsInMap(&bot))
            {
                Position position = member->GetPosition();
                if (member->isMoving() && !member->movespline->Finalized())
                {
                    G3D::Vector3 const end = member->movespline->FinalDestination();
                    position.Relocate(end.x, end.y, end.z);
                }
                if (bot.GetExactDist2d(&position) <= MarkEscapeSearch + MarkRetreatSpacing)
                    occupied.push_back(position);
            }

    auto const clearanceAt = [&bot, &markClearanceAt, &occupied, current](float x, float y, float z)
    {
        float const gap = markClearanceAt(x, y, z);
        if ((x != bot.GetPositionX() || y != bot.GetPositionY()) && gap < current + 1.0f)
            return std::numeric_limits<float>::lowest();
        float spacing = 0.0f;
        for (Position const& position : occupied)
            spacing = std::min(spacing, position.GetExactDist2d(x, y) - MarkRetreatSpacing);
        return gap + spacing;
    };

    // Break long retreats into reachable steps. Reject the full walk if it
    // crosses the encounter boundary even when the endpoint is inside.
    Position spot;
    Movement::PointsArray checkedPath;
    if (!RaidClear::FindWayOut(_ai, &bot, clearanceAt, 1.0f, 3.0f, spot, MarkEscapeSearch, 3.0f, roomPath,
        &checkedPath))
        return false;

    if (bot.IsSitState())
        bot.SetStandState(UNIT_STAND_STATE_STAND);
    bot.CastStop();
    LastMovement& movement = _ai->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get();
    movement.clear();
    // Launch the checked vertices directly, including on paths with just two points.
    bot.GetMotionMaster()->Clear();
    Movement::MoveSplineInit init(&bot);
    init.MovebyPath(checkedPath);
    init.SetWalk(false);
    int32 const duration = init.Launch();
    if (duration <= 0)
        return false;
    _markSpline = bot.movespline->GetId();
    G3D::Vector3 const end = bot.movespline->FinalDestination();
    movement.Set(bot.GetMapId(), end.x, end.y, end.z, bot.GetOrientation(),
        std::min(static_cast<float>(duration), static_cast<float>(sPlayerbotAIConfig.MaxWaitForMove)),
        MovementPriority::MOVEMENT_FORCED);
    LOG_INFO("playerbots", "[Naxx40] Horsemen mark retreat instance={} {} -> ({:.1f}, {:.1f}, {:.2f}) "
        "distance={:.1f}yd clearance={:.1f}yd", bot.GetInstanceId(), bot.GetName(), end.x, end.y, end.z,
        bot.GetExactDist2d(end.x, end.y), markClearanceAt(end.x, end.y, end.z));
    return true;
}
