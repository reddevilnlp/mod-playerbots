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
#include <optional>

namespace
{
    constexpr uint32 NPC_VOID_ZONE = 16697;
    constexpr uint32 SPELL_CONSUMPTION = 28865;
    constexpr uint32 ZoneUpdateMs = 250;
    constexpr float ZoneScanRadius = 115.0f;
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

    G3D::Vector3 Point(Position const& position)
    {
        return {position.GetPositionX(), position.GetPositionY(), position.GetPositionZ()};
    }

    G3D::Vector3 CurrentPoint(Player const& bot)
    {
        if (bot.movespline->Finalized() || !bot.movespline->HasStarted())
            return Point(bot);
        auto const current = bot.movespline->ComputePosition();
        return {current.x, current.y, current.z};
    }

    bool InsideHorsemenRoom(G3D::Vector3 const& point)
    {
        return InsideHorsemenRoom(point.x, point.y, point.z);
    }

    bool GroundLeg(Player& bot, G3D::Vector3 const& from, G3D::Vector3 const& to, Movement::PointsArray& vertices)
    {
        PathGenerator path(&bot);
        path.SetExcludeFlags(NAV_WATER | NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP);
        if (!path.CalculatePath(from.x, from.y, from.z, to.x, to.y, to.z, false) ||
            path.GetPathType() != PATHFIND_NORMAL || path.GetPath().size() < 2 ||
            (path.GetActualEndPosition() - to).length() > RaidClear::WAY_OUT_END_SLACK)
            return false;

        vertices = path.GetPath();
        vertices.front() = from;
        if (!std::ranges::all_of(vertices, [](G3D::Vector3 const& point) { return InsideHorsemenRoom(point); }))
            return false;
        for (std::size_t i = 1; i < vertices.size(); ++i)
        {
            G3D::Vector3 const& first = vertices[i - 1];
            G3D::Vector3 const& last = vertices[i];
            if (!bot.GetMap()->isInLineOfSight(first.x, first.y, first.z + 0.85f, last.x, last.y, last.z + 0.85f,
                bot.GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing))
                return false;
        }
        return true;
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

Position const* FourHorsemen40Hazards::FirstBlockedZone(Player const& bot, std::span<G3D::Vector3 const> path) const
{
    static float const radiusSquared = std::pow(ConsumptionRadius() + MovementClearance, 2.0f);
    G3D::Vector3 from = CurrentPoint(bot);
    for (G3D::Vector3 const& to : path)
    {
        G3D::Vector3 const leg = to - from;
        float const lengthSquared = leg.squaredLength();
        if (lengthSquared < 0.0001f)
            continue;
        for (Position const& zone : _voidZones)
        {
            G3D::Vector3 const offset = from - Point(zone);
            float const startSquared = offset.squaredLength();
            float const along = std::clamp(-offset.dot(leg) / lengthSquared, 0.0f, 1.0f);
            float const closestSquared = (offset + leg * along).squaredLength();
            // An underfoot zone can only be left by walking out. Permit progress
            // away from its center, but never a shortcut back through it.
            if (closestSquared < std::min(startSquared, radiusSquared) - 0.01f)
                return &zone;
        }
        from = to;
    }
    return nullptr;
}

bool FourHorsemen40Hazards::SafeSpline(Player const& bot, uint32 splineId) const
{
    if (!splineId || !bot.isMoving() || bot.movespline->Finalized() || bot.movespline->GetId() != splineId)
        return false;
    Movement::PointsArray remaining;
    auto const& spline = bot.movespline->_Spline();
    for (int32 i = bot.movespline->_currentSplineIdx() + 1; i <= spline.last(); ++i)
        remaining.push_back(spline.getPoint(i));
    return !remaining.empty() && !FirstBlockedZone(bot, remaining) &&
        std::ranges::all_of(remaining, [](G3D::Vector3 const& point) { return InsideHorsemenRoom(point); });
}

bool FourHorsemen40Hazards::Launch(Player& bot, Movement::PointsArray const& path, uint32& splineId)
{
    if (bot.IsSitState())
        bot.SetStandState(UNIT_STAND_STATE_STAND);
    bot.CastStop();
    LastMovement& movement = _ai->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get();
    movement.clear();
    bot.GetMotionMaster()->Clear();
    Movement::MoveSplineInit init(&bot);
    init.MovebyPath(path);
    init.SetWalk(false);
    int32 const duration = init.Launch();
    if (duration <= 0)
        return false;
    splineId = bot.movespline->GetId();
    G3D::Vector3 const end = bot.movespline->FinalDestination();
    movement.Set(bot.GetMapId(), end.x, end.y, end.z, bot.GetOrientation(),
        std::min(static_cast<float>(duration), static_cast<float>(sPlayerbotAIConfig.MaxWaitForMove)),
        MovementPriority::MOVEMENT_FORCED);
    return true;
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
    if (!_ai->CanMove() || !InsideHorsemenRoom(Point(bot)))
        return false;
    if (SafeSpline(bot, _rearSpline))
    {
        G3D::Vector3 const end = bot.movespline->FinalDestination();
        if (Clearance(Position(end.x, end.y, end.z)) >= MovementClearance)
            return true;
    }
    if (bot.isMoving())
        bot.StopMoving();

    uint32 const now = getMSTime();
    if (_voidRetryAt && getMSTimeDiff(_voidRetryAt, now) < RaidClear::WAY_OUT_RETRY_MS)
        return true;
    _voidRetryAt = now;

    // Keep the assigned corner once there. In transit, an escape can also move towards it.
    float const limit = std::max(cornerRadius, bot.GetExactDist2d(&corner));
    auto const clearanceAt = [this, &corner, limit](float x, float y, float z)
    {
        if (!InsideHorsemenRoom(x, y, z) || corner.GetExactDist2d(x, y) > limit)
            return std::numeric_limits<float>::lowest();
        return Clearance(Position(x, y, z));
    };
    auto const safePath = [this, &bot](Movement::PointsArray const& path)
    {
        return !FirstBlockedZone(bot, path) &&
            std::ranges::all_of(path, [](G3D::Vector3 const& point) { return InsideHorsemenRoom(point); });
    };
    Position spot;
    Movement::PointsArray path;
    float const want = MovementClearance + 0.5f;
    if (!RaidClear::FindWayOut(_ai, &bot, clearanceAt, want, 1.0f, spot, EscapeRadius, 3.0f, safePath, &path) ||
        Clearance(spot) < MovementClearance)
    {
        // A pool against the outer edge of a corner can leave no safe spot
        // inward. Keep the forced movement inside this encounter room.
        auto const roomClearanceAt = [this](float x, float y, float z)
        {
            return InsideHorsemenRoom(x, y, z) ? Clearance(Position(x, y, z)) : std::numeric_limits<float>::lowest();
        };
        if (!RaidClear::FindWayOut(_ai, &bot, roomClearanceAt, want, 1.0f, spot, EscapeRadius, 3.0f, safePath, &path))
            return true;
    }
    Launch(bot, path, _rearSpline);
    return true;
}

bool FourHorsemen40Hazards::WalkRear(Player& bot, Position const& destination, float arrival, bool detour)
{
    if (!_ai->CanMove() || !InsideHorsemenRoom(Point(bot)) || !InsideHorsemenRoom(Point(destination)))
        return false;

    RefreshVoidZones(bot);
    bool const movingSafely = SafeSpline(bot, _rearSpline);
    if (movingSafely)
    {
        G3D::Vector3 const end = bot.movespline->FinalDestination();
        if (destination.GetExactDist(end.x, end.y, end.z) <= arrival)
            return true;
    }
    else if (bot.isMoving())
        bot.StopMoving();

    if (bot.GetExactDist(destination) <= arrival || Clearance(destination) < MovementClearance)
        return false;
    uint32 const now = getMSTime();
    if (_rearRetryAt && getMSTimeDiff(_rearRetryAt, now) < RaidClear::WAY_OUT_RETRY_MS &&
        _blockedGoal.GetExactDist(destination) <= arrival)
        return false;

    G3D::Vector3 const from = CurrentPoint(bot);
    G3D::Vector3 const to = Point(destination);
    Movement::PointsArray path;
    if (!GroundLeg(bot, from, to, path))
        return false;
    RaidClear::Leash::WalkCheck const leash(_ai, &bot);
    Position const* const zone = FirstBlockedZone(bot, path);
    if (!zone)
        return !leash.Pulls(path) && Launch(bot, path, _rearSpline);
    if (!detour)
        return false;

    // The navmesh does not contain spell hazards. Try a dry dogleg around
    // the first obstruction, with both legs checked against every active pool.
    std::vector<G3D::Vector3> detours;
    for (float radius : {6.0f, 9.0f})
        for (int step = 0; step < 8; ++step)
        {
            float const angle = static_cast<float>(step) * static_cast<float>(M_PI) / 4.0f;
            float const x = zone->GetPositionX() + std::cos(angle) * radius;
            float const y = zone->GetPositionY() + std::sin(angle) * radius;
            float const z = bot.GetMapHeight(x, y, zone->GetPositionZ() + 2.0f, true, 5.0f);
            if (InsideHorsemenRoom(x, y, z) && Clearance(Position(x, y, z)) >= MovementClearance)
                detours.emplace_back(x, y, z);
        }
    std::ranges::sort(detours, {}, [&from, &to](G3D::Vector3 const& point)
    {
        return (point - from).length() + (point - to).length();
    });
    for (G3D::Vector3 const& waypoint : detours)
    {
        Movement::PointsArray first;
        Movement::PointsArray second;
        if (!GroundLeg(bot, from, waypoint, first) || FirstBlockedZone(bot, first) ||
            !GroundLeg(bot, first.back(), to, second))
            continue;
        first.insert(first.end(), std::next(second.begin()), second.end());
        if (!FirstBlockedZone(bot, first) && !leash.Pulls(first))
            return Launch(bot, first, _rearSpline);
    }

    _blockedGoal = destination;
    _rearRetryAt = now;
    return false;
}

bool FourHorsemen40Hazards::ApproachRear(Player& bot, Unit const& target, float range, float arrival)
{
    RefreshVoidZones(bot);
    auto const canTauntFrom = [&bot, &target, range](float x, float y, float z)
    {
        return InsideHorsemenRoom(x, y, z) && target.GetExactDist(x, y, z) < range &&
            bot.GetMap()->isInLineOfSight(x, y, z + 1.5f, target.GetPositionX(), target.GetPositionY(),
                target.GetPositionZ() + 1.5f, bot.GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
    };
    if (SafeSpline(bot, _rearSpline))
    {
        G3D::Vector3 const end = bot.movespline->FinalDestination();
        if (canTauntFrom(end.x, end.y, end.z))
            return true;
    }

    float const angleFromTarget = target.GetAbsoluteAngle(&bot);
    std::optional<Position> detourGoal;
    for (float distance : {range - 3.0f, range - 8.0f})
        for (int step : {0, 1, -1, 2, -2, 3, -3, 4})
        {
            float const angle = angleFromTarget + static_cast<float>(step) * static_cast<float>(M_PI) / 4.0f;
            float const x = target.GetPositionX() + std::cos(angle) * distance;
            float const y = target.GetPositionY() + std::sin(angle) * distance;
            float const z = bot.GetMapHeight(x, y, bot.GetPositionZ() + 2.0f, true, 5.0f);
            Position const position(x, y, z);
            if (!canTauntFrom(x, y, z) || Clearance(position) < MovementClearance)
                continue;
            if (WalkRear(bot, position, arrival, false))
                return true;
            if (!detourGoal)
                detourGoal = position;
        }
    return detourGoal && WalkRear(bot, *detourGoal, arrival);
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
    auto const roomPath = [this, &bot](auto const& path)
    {
        if (path.empty() || FirstBlockedZone(bot, path))
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
        if (SafeSpline(bot, _markSpline) && (endClearance >= 0.0f ||
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
        if (outsideRoom || current >= 0.0f || bot.movespline->GetId() == _markSpline)
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

    // Launch the checked vertices directly, including on paths with just two points.
    if (!Launch(bot, checkedPath, _markSpline))
        return false;
    G3D::Vector3 const end = bot.movespline->FinalDestination();
    LOG_INFO("playerbots", "[Naxx40] Horsemen mark retreat instance={} {} -> ({:.1f}, {:.1f}, {:.2f}) "
        "distance={:.1f}yd clearance={:.1f}yd", bot.GetInstanceId(), bot.GetName(), end.x, end.y, end.z,
        bot.GetExactDist2d(end.x, end.y), markClearanceAt(end.x, end.y, end.z));
    return true;
}
