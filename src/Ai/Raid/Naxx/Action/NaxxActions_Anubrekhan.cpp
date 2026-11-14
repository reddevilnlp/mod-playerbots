/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NaxxActions.h"
#include "Ai/Raid/Naxx40/Naxx40Encounter.h"
#include "Log.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectGuid.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "Spell.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr uint32 AnubEntry = 351009;
constexpr uint32 LocustSwarm = 28785;
constexpr float RoomCenterX = 3272.49f;
constexpr float RoomCenterY = -3476.27f;
constexpr float SafeDistance = 18.0f;
constexpr float BacklineDistance = 22.0f;
constexpr float TankRingRadius = 37.0f;
constexpr float RoomRadius = 50.0f;

bool MovesAwayFrom(Unit const& boss, Movement::PointsArray const& points, float startRange)
{
    float const minimum = std::min(startRange, SafeDistance) - 0.75f;
    G3D::Vector3 const origin(boss.GetPositionX(), boss.GetPositionY(), 0.0f);
    for (std::size_t i = 1; i < points.size(); ++i)
    {
        G3D::Vector3 const from(points[i - 1].x, points[i - 1].y, 0.0f);
        G3D::Vector3 const to(points[i].x, points[i].y, 0.0f);
        G3D::Vector3 const segment = to - from;
        if (segment.squaredLength() == 0.0f)
            continue;
        float const fraction = std::clamp((origin - from).dot(segment) / segment.squaredLength(), 0.0f, 1.0f);
        if ((from + segment * fraction - origin).squaredLength() < minimum * minimum && minimum > 0.0f)
            return false;
    }
    return true;
}
}

bool Anub40::HasLocustSwarm(Unit const& boss)
{
    Spell const* spell = boss.GetCurrentSpell(CURRENT_GENERIC_SPELL);
    return boss.HasAura(LocustSwarm) || (spell && spell->GetSpellInfo()->Id == LocustSwarm);
}

bool Anub40::IsActiveBoss(Unit const* boss)
{
    return boss && boss->GetEntry() == AnubEntry && boss->IsAlive() && boss->IsInCombat();
}

bool AnubrekhanChooseTargetAction::Execute(Event /*event*/)
{
    GuidVector attackers = context->GetValue<GuidVector>("attackers")->Get();
    Unit* target = nullptr;
    Unit* target_boss = nullptr;
    std::vector<Unit*> target_guards;
    for (ObjectGuid const guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit)
            continue;
        if (botAI->EqualLowercaseName(unit->GetName(), "crypt guard"))
            target_guards.push_back(unit);

        if (botAI->EqualLowercaseName(unit->GetName(), "anub'rekhan"))
            target_boss = unit;
    }
    if (Anub40::IsActiveBoss(target_boss) && Anub40::HasLocustSwarm(*target_boss))
        return false;

    if (PlayerbotAI::IsMainTank(bot))
        target = target_boss;
    else
    {
        if (target_guards.size() == 0)
            target = target_boss;
        else
        {
            if (PlayerbotAI::IsAssistTank(bot))
            {
                for (Unit* t : target_guards)
                {
                    if (target == nullptr || (target->GetVictim() && target->GetVictim()->ToPlayer() &&
                                              PlayerbotAI::IsTank(target->GetVictim()->ToPlayer())))
                        target = t;
                }
            }
            else
            {
                for (Unit* t : target_guards)
                {
                    if (target == nullptr || target->GetHealthPct() > t->GetHealthPct())
                        target = t;
                }
            }
        }
    }
    if (context->GetValue<Unit*>("current target")->Get() == target)
        return false;

    return Attack(target);
}

bool AnubrekhanPositionAction::Execute(Event /*event*/)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "anub'rekhan");
    if (!boss)
        return false;

    if (IsNaxx40Map(bot->FindMap()))
        return Anub40::IsActiveBoss(boss) && PositionNaxx40(*boss);

    bool inPhase = botAI->HasAura("locust swarm", boss) || boss->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (inPhase)
    {
        if (PlayerbotAI::IsMainTank(bot))
        {
            uint32 nearest = FindNearestWaypoint();
            uint32 next_point;
            next_point = (nearest + 1) % intervals;

            return MoveTo(bot->GetMapId(), waypoints[next_point].first, waypoints[next_point].second,
                          bot->GetPositionZ(), false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
        }
        else
            return MoveInside(533, 3272.49f, -3476.27f, bot->GetPositionZ(), 3.0f, MovementPriority::MOVEMENT_COMBAT);
    }
    return false;
}

bool AnubrekhanPositionAction::PositionNaxx40(Unit& boss)
{
    bool const swarm = Anub40::HasLocustSwarm(boss);
    bool const tank = boss.GetVictim() == bot && PlayerbotAI::IsTank(bot);
    bool const backline = PlayerbotAI::IsRanged(bot) || PlayerbotAI::IsHeal(bot);
    float const distance = bot->GetExactDist2d(&boss);
    MovementPriority const priority = swarm ? MovementPriority::MOVEMENT_FORCED : MovementPriority::MOVEMENT_COMBAT;
    if ((!swarm && (tank || !backline || distance >= SafeDistance)) ||
        (swarm && distance >= (tank ? SafeDistance : SafeDistance - 1.0f)) ||
        !botAI->CanMove() || !IsMovingAllowed() || bot->IsFalling() || IsWaitingForLastMove(priority))
        return false;

    float const roundedAngle = boss.GetAbsoluteAngle(bot);
    float const angle = backline && !tank
        ? roundedAngle + (static_cast<int32>(bot->GetGUID().GetRawValue() % 5) - 2) * 0.10f
        : roundedAngle;
    auto launch = [&](float x, float y)
    {
        if (std::hypot(x - RoomCenterX, y - RoomCenterY) > RoomRadius || boss.GetExactDist2d(x, y) < SafeDistance)
            return false;
        float const z = bot->GetMapHeight(x, y, bot->GetPositionZ() + 2.0f, true, 8.0f);
        if (std::abs(z - bot->GetPositionZ()) > 2.0f)
            return false;

        PathGenerator path(bot);
        path.SetExcludeFlags(NAV_WATER | NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP);
        if (!path.CalculatePath(x, y, z) || path.GetPathType() != PATHFIND_NORMAL || path.GetPath().size() < 2 ||
            (path.GetActualEndPosition() - G3D::Vector3(x, y, z)).squaredLength() > 2.25f ||
            path.getPathLength() > 36.0f || !MovesAwayFrom(boss, path.GetPath(), distance))
            return false;

        if (swarm && bot->IsNonMeleeSpellCast(true))
            bot->CastStop();
        Movement::PointsArray points = path.GetPath();
        if (points.size() == 2)
            points.insert(points.begin() + 1, (points.front() + points.back()) * 0.5f);
        uint32 const before = bot->movespline->GetId();
        bot->GetMotionMaster()->MoveSplinePath(&points, FORCED_MOVEMENT_RUN);
        if (bot->movespline->Finalized() || bot->movespline->GetId() == before)
            return false;
        AI_VALUE(LastMovement&, "last movement").Set(bot->GetMapId(), x, y, z, bot->GetOrientation(),
            bot->movespline->Duration(), priority);
        if (swarm)
            LOG_INFO("playerbots", "[Naxx40] Anub Locust Swarm move bot={} instance={} bossDist={:.1f} tank={}",
                bot->GetName(), bot->GetInstanceId(), distance, tank);
        return true;
    };

    if (tank)
    {
        // The room's 37-yard floor ring is dry. The legacy 45-yard ring reaches walls and the pool.
        constexpr float RingStep = static_cast<float>(M_PI) / 8.0f;
        float const current = std::atan2(bot->GetPositionY() - RoomCenterY, bot->GetPositionX() - RoomCenterX);
        float const nearest = std::round(current / RingStep) * RingStep;
        for (int step : {1, 2, -1, 3, -2})
        {
            float const next = nearest + step * RingStep;
            if (launch(RoomCenterX + TankRingRadius * std::cos(next), RoomCenterY + TankRingRadius * std::sin(next)))
                return true;
        }
    }

    // Follow the live boss position, maintaining separation even if he has already moved through the centre.
    for (float offset : {0.0f, 0.52f, -0.52f, 1.05f, -1.05f, 1.57f, -1.57f})
        if (launch(boss.GetPositionX() + BacklineDistance * std::cos(angle + offset),
                   boss.GetPositionY() + BacklineDistance * std::sin(angle + offset)))
            return true;
    return false;
}
