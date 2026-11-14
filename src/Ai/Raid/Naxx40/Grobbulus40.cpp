/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Grobbulus40.h"

#include <algorithm>
#include <cmath>

#include "Creature.h"
#include "InstanceScript.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "Multiplier.h"
#include "Naxx40Encounter.h"
#include "NaxxActions.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "ReachTargetActions.h"
#include "SpellAuraEffects.h"
#include "TemporarySummon.h"
#include "Timer.h"

namespace
{
constexpr char StrategyName[] = "naxx40 grobbulus";
constexpr char CloudDanger[] = "grobbulus 40 cloud danger";
constexpr char LeaveCloud[] = "grobbulus 40 leave cloud";
constexpr uint32 GrobbulusEncounter = 1;
constexpr uint32 GrobbulusEntry = 351003;
constexpr uint32 PoisonAura = 28158;
constexpr uint32 HeroicPoisonAura = 54362;
constexpr uint32 MutatingInjection = 28169;
constexpr uint32 FreshCloudTimer = 73000;
constexpr uint32 SnapshotInterval = 250;
constexpr float BossSearchRadius = 240.0f;
constexpr float CloudSearchRadius = 140.0f;
constexpr float EscapeMargin = 1.5f;
constexpr float Pi = 3.14159265f;
Position const ArenaCenter(3281.23f, -3310.38f, 0.0f);
std::vector<uint32> const CloudEntries = {16363, 351089};

struct Cloud
{
    G3D::Vector3 center;
    float radius;
};

G3D::Vector3 Location(Position const& position)
{
    return {position.GetPositionX(), position.GetPositionY(), position.GetPositionZ()};
}

std::vector<Cloud> const* ObserveClouds(Player* bot)
{
    if (!bot->IsAlive() || !IsNaxx40Map(bot->FindMap()))
        return nullptr;

    InstanceScript const* instance = bot->GetInstanceScript();
    if (!instance)
        return nullptr;
    bool const postKill = instance->IsBossDone(GrobbulusEncounter);
    if (postKill ? !Naxx40GrobbulusClouds::OnRoomFloor(*bot)
                 : !bot->IsInCombat() || instance->GetBossState(GrobbulusEncounter) != IN_PROGRESS)
        return nullptr;

    // The map updates serially on its worker. Share the scan across the raid without retaining world object pointers.
    thread_local uint32 instanceId = 0;
    thread_local uint32 sampledAt = 0;
    thread_local bool sampledPostKill = false;
    thread_local std::vector<Cloud> clouds;
    uint32 const now = getMSTime();
    if (sampledAt && instanceId == bot->GetInstanceId() && sampledPostKill == postKill &&
        now - sampledAt < SnapshotInterval)
        return &clouds;

    Creature const* boss = postKill ? nullptr : bot->FindNearestCreature(GrobbulusEntry, BossSearchRadius);
    if (!postKill && (!boss || !boss->IsInCombat()))
        return nullptr;

    instanceId = bot->GetInstanceId();
    sampledAt = now;
    sampledPostKill = postKill;
    clouds.clear();
    std::list<Creature*> nearby;
    if (postKill)
        bot->GetCreatureListWithEntryInGrid(nearby, CloudEntries, 200.0f);
    else
        boss->GetCreatureListWithEntryInGrid(nearby, CloudEntries, CloudSearchRadius);
    for (Creature* cloud : nearby)
    {
        if (!cloud->IsAlive() || (postKill && !bot->InSamePhase(cloud)))
            continue;
        AuraEffect const* aura = cloud->GetAuraEffect(PoisonAura, EFFECT_0);
        if (!aura)
            aura = cloud->GetAuraEffect(HeroicPoisonAura, EFFECT_0);
        if (aura)
        {
            // SpellAuraEffects scales the 30-yard base radius to 2 + 0.3 * tick, in exact 3D distance.
            float const nextRadius = 2.0f + 0.3f * static_cast<float>(aura->GetTickNumber() + 1);
            clouds.push_back({Location(*cloud), nextRadius + EscapeMargin});
        }
        // Newly summoned clouds apply their damage aura after the first timer tick.
        // An older aura-less shell has already stopped doing damage.
        else if (postKill)
        {
            if (TempSummon* summon = cloud->ToTempSummon(); summon && summon->GetTimer() >= FreshCloudTimer)
                clouds.push_back({Location(*cloud), 2.6f + EscapeMargin});
        }
    }
    return &clouds;
}

Cloud const* DeepestCloud(G3D::Vector3 const& point, std::vector<Cloud> const& clouds)
{
    Cloud const* deepest = nullptr;
    float penetration = 0.0f;
    for (Cloud const& cloud : clouds)
    {
        float const depth = cloud.radius - (point - cloud.center).length();
        if (depth > penetration)
        {
            deepest = &cloud;
            penetration = depth;
        }
    }
    return deepest;
}

bool SegmentEntersCloud(G3D::Vector3 const& from, G3D::Vector3 const& to, std::vector<Cloud> const& clouds)
{
    G3D::Vector3 const step = to - from;
    float const lengthSq = step.squaredLength();
    if (lengthSq < 0.001f)
        return false;

    for (Cloud const& cloud : clouds)
    {
        G3D::Vector3 const offset = from - cloud.center;
        float const radiusSq = cloud.radius * cloud.radius;
        float const closest = std::clamp(-offset.dot(step) / lengthSq, 0.0f, 1.0f);
        // A bot already inside can walk outward, including along a tangent. Allow for small height corrections.
        if (offset.squaredLength() < radiusSq)
        {
            float const closestDistance = (offset + step * closest).length();
            if (offset.length() - closestDistance > 0.25f)
                return true;
        }
        else if ((offset + step * closest).squaredLength() < radiusSq)
            return true;
    }
    return false;
}

bool NeedsEscape(Player const& bot, std::vector<Cloud> const& clouds)
{
    if (clouds.empty())
        return false;

    G3D::Vector3 from = Location(bot);
    Movement::MoveSpline const& walk = *bot.movespline;
    if (walk.Finalized())
        return DeepestCloud(from, clouds) != nullptr;

    if (DeepestCloud(walk.FinalDestination(), clouds))
        return true;

    auto const& spline = walk._Spline();
    for (int32 index = walk._currentSplineIdx() + 1; index <= spline.last(); ++index)
    {
        G3D::Vector3 const& to = spline.getPoint(index);
        if (SegmentEntersCloud(from, to, clouds))
            return true;
        from = to;
    }
    return false;
}

class Grobbulus40CloudTrigger final : public Trigger
{
public:
    explicit Grobbulus40CloudTrigger(PlayerbotAI* ai) : Trigger(ai, CloudDanger) {}

    bool IsActive() override
    {
        auto const* clouds = ObserveClouds(bot);
        return clouds && NeedsEscape(*bot, *clouds);
    }
};

class Grobbulus40EscapeAction final : public MovementAction
{
public:
    explicit Grobbulus40EscapeAction(PlayerbotAI* ai) : MovementAction(ai, LeaveCloud) {}

    bool Execute(Event /*event*/) override
    {
        auto const* clouds = ObserveClouds(bot);
        if (!clouds || !NeedsEscape(*bot, *clouds) || !IsMovingAllowed())
            return false;

        G3D::Vector3 const start = Location(*bot);
        if (Cloud const* danger = DeepestCloud(start, *clouds))
            return Escape(start, *danger, *clouds);

        // A normal combat move can point back into a cloud. Cancel that spline while still outside it.
        bot->GetMotionMaster()->MovementExpiredOnSlot(MOTION_SLOT_ACTIVE, false);
        bot->StopMoving();
        return true;
    }

private:
    bool Escape(G3D::Vector3 const& start, Cloud const& danger, std::vector<Cloud> const& clouds)
    {
        float const away = std::atan2(start.y - danger.center.y, start.x - danger.center.x);
        float const depth = danger.radius - (start - danger.center).length();
        PathGenerator path(bot);
        path.SetPathLengthLimit(32.0f);
        if (bot->GetInstanceScript()->IsBossDone(GrobbulusEncounter))
            path.SetExcludeFlags(NAV_WATER | NAV_MAGMA | NAV_SLIME | NAV_GROUND_STEEP);
        for (float const distance : {depth + 3.0f, depth + 7.0f})
        {
            for (float const offset : {0.0f, Pi / 6.0f, -Pi / 6.0f, Pi / 3.0f, -Pi / 3.0f, Pi / 2.0f, -Pi / 2.0f})
            {
                float const angle = away + offset;
                G3D::Vector3 dest(start.x + distance * std::cos(angle), start.y + distance * std::sin(angle), start.z);
                bot->UpdateAllowedPositionZ(dest.x, dest.y, dest.z);
                if (std::abs(dest.z - start.z) > 3.0f || DeepestCloud(dest, clouds) ||
                    SegmentEntersCloud(start, dest, clouds))
                    continue;

                if (!path.CalculatePath(dest.x, dest.y, dest.z) || path.GetPathType() != PATHFIND_NORMAL)
                    continue;

                Movement::PointsArray points = path.GetPath();
                if (points.size() < 2 || DeepestCloud(points.back(), clouds))
                    continue;

                G3D::Vector3 from = start;
                bool safe = true;
                // The spline replaces the first point with the bot's current position on launch.
                for (std::size_t index = 1; index < points.size(); ++index)
                {
                    G3D::Vector3 const& to = points[index];
                    if (SegmentEntersCloud(from, to, clouds))
                    {
                        safe = false;
                        break;
                    }
                    from = to;
                }
                if (!safe)
                    continue;

                // Two-point escort paths get recalculated by the core. Keep the checked path intact.
                if (points.size() == 2)
                    points.insert(points.begin() + 1, (points.front() + points.back()) * 0.5f);

                G3D::Vector3 const& end = points.back();
                if (bot->IsSitState())
                    bot->SetStandState(UNIT_STAND_STATE_STAND);
                bot->CastStop();
                bot->GetMotionMaster()->MoveSplinePath(&points, FORCED_MOVEMENT_RUN);
                float const delay = std::clamp(IN_MILLISECONDS * MoveDelay(path.getPathLength()) -
                                                   static_cast<float>(botAI->GetReactDelay()), 0.0f,
                                               static_cast<float>(sPlayerbotAIConfig.MaxWaitForMove));
                AI_VALUE(LastMovement&, "last movement")
                    .Set(bot->GetMapId(), end.x, end.y, end.z, bot->GetOrientation(), delay,
                         MovementPriority::MOVEMENT_FORCED);
                return true;
            }
        }
        return false;
    }
};

class Grobbulus40CloudMultiplier final : public Multiplier
{
public:
    explicit Grobbulus40CloudMultiplier(PlayerbotAI* ai) : Multiplier(ai, "grobbulus 40 cloud avoidance") {}

    float GetValue(Action* action) override
    {
        if (!dynamic_cast<MovementAction*>(action) || dynamic_cast<Grobbulus40EscapeAction*>(action))
            return 1.0f;

        auto const* clouds = ObserveClouds(bot);
        if (!clouds || clouds->empty())
            return 1.0f;

        G3D::Vector3 const start = Location(*bot);
        if (DeepestCloud(start, *clouds))
            return 0.0f;

        if (dynamic_cast<GrobbulusMoveCenterAction*>(action))
        {
            G3D::Vector3 const center(ArenaCenter.GetPositionX(), ArenaCenter.GetPositionY(), start.z);
            return SegmentEntersCloud(start, center, *clouds) ? 0.0f : 1.0f;
        }

        if (dynamic_cast<ReachMeleeAction*>(action))
        {
            if (Unit const* target = AI_VALUE(Unit*, "current target"))
            {
                G3D::Vector3 const toward = Location(*target) - start;
                float const distance = toward.length();
                float const range = sPlayerbotAIConfig.MeleeDistance + bot->GetCombatReach() + target->GetCombatReach();
                if (distance > range && SegmentEntersCloud(start, start + toward * (1.0f - range / distance), *clouds))
                    return 0.0f;
            }
        }
        return 1.0f;
    }
};

class Grobbulus40Strategy final : public Strategy
{
public:
    explicit Grobbulus40Strategy(PlayerbotAI* ai) : Strategy(ai) {}
    std::string const getName() override { return StrategyName; }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override
    {
        triggers.push_back(new TriggerNode(CloudDanger, {NextAction(LeaveCloud, ACTION_EMERGENCY + 1)}));
    }

    void InitMultipliers(std::vector<Multiplier*>& multipliers) override
    {
        multipliers.push_back(new Grobbulus40CloudMultiplier(botAI));
    }
};
}  // namespace

bool Naxx40GrobbulusClouds::OnRoomFloor(Player const& player)
{
    return player.GetPositionZ() >= 290.0f && player.GetPositionZ() <= 295.0f &&
        player.GetPositionX() >= 3189.0f && player.GetPositionX() <= 3337.0f &&
        player.GetPositionY() >= -3340.0f && player.GetPositionY() <= -3258.0f;
}

bool Naxx40GrobbulusClouds::IsEntry(uint32 entry)
{
    return entry == CloudEntries[0] || entry == CloudEntries[1];
}

bool Naxx40GrobbulusClouds::HasPendingInjection(Player const& player)
{
    return OnRoomFloor(player) && player.HasAura(MutatingInjection);
}

bool Naxx40GrobbulusClouds::NeedsEscape(Player& player)
{
    auto const* clouds = ObserveClouds(&player);
    return clouds && (::NeedsEscape(player, *clouds) || DeepestCloud(Location(player), *clouds));
}

bool Naxx40GrobbulusClouds::ClearsPath(Player& player, Movement::PointsArray const& path)
{
    auto const* clouds = ObserveClouds(&player);
    if (!clouds || path.empty())
        return true;
    for (std::size_t i = 1; i < path.size(); ++i)
        if (SegmentEntersCloud(path[i - 1], path[i], *clouds))
            return false;
    return !DeepestCloud(path.back(), *clouds);
}

Grobbulus40ActionContext::Grobbulus40ActionContext()
{
    creators[LeaveCloud] = [](PlayerbotAI* ai) { return new Grobbulus40EscapeAction(ai); };
}

Grobbulus40TriggerContext::Grobbulus40TriggerContext()
{
    creators[CloudDanger] = [](PlayerbotAI* ai) { return new Grobbulus40CloudTrigger(ai); };
}

Grobbulus40StrategyContext::Grobbulus40StrategyContext()
{
    creators[StrategyName] = [](PlayerbotAI* ai) { return new Grobbulus40Strategy(ai); };
}
