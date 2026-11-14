/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Naxx40Slime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include "MotionMaster.h"
#include "MoveSpline.h"
#include "MovementActions.h"
#include "Naxx40Encounter.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "Timer.h"

namespace
{
constexpr char InSlime[] = "naxx40 in slime";
constexpr char LeaveSlime[] = "naxx40 leave slime";
constexpr char MovingNearSlime[] = "naxx40 moving near slime";
constexpr char TakeDryPath[] = "naxx40 take dry path";
constexpr char AuthorizedCrossing[] = "naxx40 slime crossing";
constexpr char SlimeEscape[] = "naxx40 slime escape";
constexpr uint32 TriggerIntervalMs = 250;
constexpr uint32 SearchIntervalMs = 750;
constexpr uint32 CrossingGraceMs = 500;
constexpr uint32 CrossingLimitMs = 8000;
constexpr float SampleStep = 0.75f;
constexpr float DryMargin = 1.25f;
constexpr float MaxBankHeightChange = 4.0f;
constexpr float ExitPathLimit = 32.0f;
constexpr float QuickExitSeconds = 2.0f;
constexpr float QuickExitLookahead = 18.0f;
constexpr float AlternateDestinationLimit = 20.0f;
constexpr float AlternatePathAllowance = 8.0f;
constexpr float AlternateLookaheadSeconds = 1.5f;
constexpr float AlternateSearchRadius = 3.0f;
constexpr float AlternateWaterCost = 25.0f;
constexpr int SearchDirections = 12;
constexpr std::array SearchRadii = {2.5f, 5.0f, 8.0f, 12.0f, 18.0f};

struct SlimeMovement
{
    uint32 instanceId = 0;
    uint32 splineId = 0;
    uint32 startedAt = 0;
    uint32 lifetime = 0;
};

bool ActiveSlimeSpline(Player const& bot)
{
    return bot.IsAlive() && IsNaxx40Map(bot.FindMap()) && !bot.movespline->Finalized() &&
           bot.GetMotionMaster()->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE;
}

void RecordSlimeSpline(PlayerbotAI& ai, char const* key)
{
    Player const* bot = ai.GetBot();
    if (!bot || !ActiveSlimeSpline(*bot))
        return;

    Movement::MoveSpline const& movement = *bot->movespline;
    uint32 const duration = static_cast<uint32>(std::max(0, movement.Duration() - movement.timePassed()));
    uint32 const lifetime = std::min(duration + CrossingGraceMs, CrossingLimitMs);
    ai.GetAiObjectContext()->GetValue<SlimeMovement>(key)->Set(
        {bot->GetInstanceId(), movement.GetId(), getMSTime(), lifetime});
}

bool IsSlimeSplineActive(PlayerbotAI& ai, char const* key)
{
    Player const* bot = ai.GetBot();
    if (!bot || !ActiveSlimeSpline(*bot))
        return false;

    SlimeMovement const saved = ai.GetAiObjectContext()->GetValue<SlimeMovement>(key)->Get();
    return saved.instanceId == bot->GetInstanceId() && saved.splineId == bot->movespline->GetId() &&
           getMSTime() - saved.startedAt < saved.lifetime;
}

G3D::Vector3 Location(WorldObject const& object)
{
    return {object.GetPositionX(), object.GetPositionY(), object.GetPositionZ()};
}

bool InDamagingSlime(Player const& bot)
{
    if (!bot.IsAlive() || bot.IsFlying() || !IsNaxx40Map(bot.FindMap()))
        return false;

    LiquidData const& liquid = bot.GetLiquidData();
    return liquid.Entry == Naxx40SlimeLiquidId && (liquid.Status & MAP_LIQUID_STATUS_SWIMMING);
}

bool TouchesSlime(Player const& bot, G3D::Vector3 const& position)
{
    PositionFullTerrainStatus terrain;
    bot.GetMap()->GetFullTerrainStatusForPosition(bot.GetPhaseMask(), position.x, position.y, position.z,
                                                  bot.GetCollisionHeight(), terrain);
    LiquidData const& liquid = terrain.liquidInfo;
    return liquid.Entry == Naxx40SlimeLiquidId && (liquid.Status & MAP_LIQUID_STATUS_IN_CONTACT);
}

template <class CheckSample>
bool CheckPath(G3D::Vector3 from, Movement::PointsArray const& points, float lookahead, CheckSample checkSample)
{
    float travelled = 0.0f;
    for (std::size_t index = 1; index < points.size(); ++index)
    {
        G3D::Vector3 const step = points[index] - from;
        float const length = step.length();
        int const samples = static_cast<int>(std::ceil(length / SampleStep));
        for (int sample = 1; sample <= samples; ++sample)
        {
            float const fraction = static_cast<float>(sample) / static_cast<float>(samples);
            float const distance = travelled + length * fraction;
            if (distance > lookahead)
                return true;
            if (!checkSample(from + step * fraction, distance))
                return false;
        }
        from = points[index];
        travelled += length;
    }
    return true;
}

// Wet movement may continue until the first dry sample. After that the path must stay clear of the bank.
bool HasDryExit(Player const& bot, Movement::PointsArray const& points, float wetLimit, float lookahead)
{
    float firstDry = -1.0f;
    float lastSample = 0.0f;
    bool const clear = CheckPath(Location(bot), points, lookahead,
                                 [&](G3D::Vector3 const& position, float distance)
                                 {
                                     lastSample = distance;
                                     if (TouchesSlime(bot, position))
                                         return firstDry < 0.0f && distance <= wetLimit;

                                     if (firstDry < 0.0f)
                                         firstDry = distance;
                                     return firstDry <= wetLimit;
                                 });
    return clear && firstDry >= 0.0f && lastSample - firstDry >= DryMargin;
}

bool HasOrdinarySpline(Player const& bot)
{
    Movement::MoveSpline const& movement = *bot.movespline;
    return !movement.Finalized() && !movement.isCyclic() && !movement.isFalling() && !movement.onTransport;
}

Movement::PointsArray RemainingSpline(Player const& bot, float lookahead)
{
    Movement::MoveSpline const& movement = *bot.movespline;
    Movement::PointsArray points{Location(bot)};
    auto const& spline = movement._Spline();
    float length = 0.0f;
    for (int32 index = movement._currentSplineIdx() + 1; index <= spline.last() && length < lookahead; ++index)
    {
        auto const& next = spline.getPoint(index);
        length += (next - points.back()).length();
        points.push_back(next);
    }
    return points;
}

bool IsLeavingSlime(Player const& bot)
{
    Movement::MoveSpline const& movement = *bot.movespline;
    if (!HasOrdinarySpline(bot) || TouchesSlime(bot, movement.FinalDestination()))
        return false;
    float const speed = std::min(movement.Velocity(), bot.GetSpeed(MOVE_SWIM));
    return HasDryExit(bot, RemainingSpline(bot, QuickExitLookahead), QuickExitSeconds * speed, QuickExitLookahead);
}

G3D::Vector3 NearbyFloor(Player const& bot, G3D::Vector3 const& position)
{
    float const z =
        bot.GetMapHeight(position.x, position.y, position.z + MaxBankHeightChange, true, 2.0f * MaxBankHeightChange);
    return {position.x, position.y, z};
}

G3D::Vector3 RadialPoint(G3D::Vector3 const& origin, int direction, float radius)
{
    float const angle = static_cast<float>(direction) * 2.0f * std::numbers::pi_v<float> / SearchDirections;
    return {origin.x + radius * std::cos(angle), origin.y + radius * std::sin(angle), origin.z};
}

Movement::PointsArray FindDryExit(Player& bot)
{
    G3D::Vector3 const start = Location(bot);
    PathGenerator path(&bot);
    path.SetPathLengthLimit(ExitPathLimit);
    Movement::PointsArray best;
    float bestLength = ExitPathLimit;
    for (float const radius : SearchRadii)
    {
        if (radius >= bestLength)
            break;

        for (int direction = 0; direction < SearchDirections; ++direction)
        {
            G3D::Vector3 const bank = NearbyFloor(bot, RadialPoint(start, direction, radius));
            if (std::abs(bank.z - start.z) > MaxBankHeightChange || TouchesSlime(bot, bank) ||
                !path.CalculatePath(bank.x, bank.y, bank.z) || path.GetPathType() != PATHFIND_NORMAL)
                continue;

            auto const& points = path.GetPath();
            float const length = path.getPathLength();
            if (length >= bestLength || points.size() < 2 || (points.back() - bank).squaredLength() > 1.0f ||
                !HasDryExit(bot, points, ExitPathLimit, ExitPathLimit))
                continue;

            best = points;
            bestLength = length;
        }
    }
    return best;
}

bool AllDry(Player const& bot, Movement::PointsArray const& points, float length)
{
    return CheckPath(Location(bot), points, length,
                     [&](G3D::Vector3 const& position, float) { return !TouchesSlime(bot, position); });
}

Movement::PointsArray DryPathTo(Player& bot, G3D::Vector3 const& destination, float limit)
{
    PathGenerator path(&bot);
    path.SetPathLengthLimit(ExitPathLimit);
    path.SetNavTerrainCost(NAV_WATER, AlternateWaterCost);
    if (!path.CalculatePath(destination.x, destination.y, destination.z) || path.GetPathType() != PATHFIND_NORMAL)
        return {};

    auto const& points = path.GetPath();
    float const length = path.getPathLength();
    if (points.size() < 2 || length < DryMargin || length > limit ||
        (points.back() - destination).squaredLength() > 1.0f || !AllDry(bot, points, ExitPathLimit))
        return {};
    return points;
}

bool CanTakeDryPath(Player const& bot)
{
    if (!bot.IsAlive() || bot.IsFlying() || !IsNaxx40Map(bot.FindMap()) || !HasOrdinarySpline(bot) ||
        InDamagingSlime(bot))
        return false;

    // Scripted escorts own their movements, including deliberate liquid crossings.
    switch (bot.GetMotionMaster()->GetCurrentMovementGeneratorType())
    {
        case POINT_MOTION_TYPE:
        case CHASE_MOTION_TYPE:
        case FOLLOW_MOTION_TYPE:
            return (bot.movespline->FinalDestination() - Location(bot)).squaredLength() <=
                   AlternateDestinationLimit * AlternateDestinationLimit;
        default:
            return false;
    }
}

Movement::PointsArray FindDryCombatPosition(Player& bot, Unit const& target, G3D::Vector3 const& destination,
                                            float range, float limit)
{
    Movement::PointsArray best;
    float bestLength = limit;
    for (int direction = 0; direction < SearchDirections; ++direction)
    {
        G3D::Vector3 const bank = NearbyFloor(bot, RadialPoint(destination, direction, AlternateSearchRadius));
        if (std::abs(bank.z - destination.z) > MaxBankHeightChange ||
            (Location(target) - bank).squaredLength() > range * range || TouchesSlime(bot, bank) ||
            !target.IsWithinLOS(bank.x, bank.y, bank.z))
            continue;

        Movement::PointsArray path = DryPathTo(bot, bank, bestLength);
        if (path.empty())
            continue;

        bestLength = 0.0f;
        for (std::size_t index = 1; index < path.size(); ++index)
            bestLength += (path[index] - path[index - 1]).length();
        best = std::move(path);
    }
    return best;
}

bool StartCheckedMovement(Player& bot, Movement::PointsArray& points)
{
    if (points.empty())
        return false;

    // The core regenerates two-point escort paths. Preserve the path we checked.
    if (points.size() == 2)
        points.insert(points.begin() + 1, (points.front() + points.back()) * 0.5f);

    if (bot.IsSitState())
        bot.SetStandState(UNIT_STAND_STATE_STAND);
    if (bot.IsNonMeleeSpellCast(true))
        bot.CastStop();

    MotionMaster* motion = bot.GetMotionMaster();
    uint32 const previousSpline = bot.movespline->GetId();
    motion->MoveSplinePath(&points, FORCED_MOVEMENT_RUN);
    return motion->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE && !bot.movespline->Finalized() &&
           bot.movespline->GetId() != previousSpline;
}

void RecordMovement(Player const& bot, LastMovement& movement, G3D::Vector3 const& end, MovementPriority priority)
{
    float const delay =
        std::min(static_cast<float>(bot.movespline->Duration()), static_cast<float>(sPlayerbotAIConfig.MaxWaitForMove));
    movement.Set(bot.GetMapId(), end.x, end.y, end.z, bot.GetOrientation(), delay, priority);
}

class Naxx40InSlimeTrigger final : public Trigger
{
public:
    explicit Naxx40InSlimeTrigger(PlayerbotAI* ai) : Trigger(ai, InSlime, TriggerIntervalMs) {}

    bool IsActive() override { return InDamagingSlime(*bot); }
};

class Naxx40LeaveSlimeAction final : public MovementAction
{
public:
    explicit Naxx40LeaveSlimeAction(PlayerbotAI* ai) : MovementAction(ai, LeaveSlime) {}

    bool isUseful() override { return InDamagingSlime(*bot) && getMSTime() - _lastSearch >= SearchIntervalMs; }

    bool isPossible() override
    {
        return IsMovingAllowed() && !bot->GetVehicle() && !bot->GetTransport() && !bot->IsFalling() &&
               !bot->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) && !IsNaxx40SlimeCrossingAuthorized(*botAI) &&
                !IsNaxx40SlimeEscapeActive(*botAI) && !IsLeavingSlime(*bot);
    }

    bool Execute(Event /*event*/) override
    {
        _lastSearch = getMSTime();
        Movement::PointsArray points = FindDryExit(*bot);
        if (!StartCheckedMovement(*bot, points))
            return false;

        RecordMovement(*bot, AI_VALUE(LastMovement&, "last movement"), points.back(),
                       MovementPriority::MOVEMENT_FORCED);
        RecordSlimeSpline(*botAI, SlimeEscape);
        return true;
    }

private:
    uint32 _lastSearch = 0;
};

class Naxx40MovingNearSlimeTrigger final : public Trigger
{
public:
    explicit Naxx40MovingNearSlimeTrigger(PlayerbotAI* ai) : Trigger(ai, MovingNearSlime, TriggerIntervalMs) {}

    bool IsActive() override { return CanTakeDryPath(*bot); }
};

class Naxx40TakeDryPathAction final : public MovementAction
{
public:
    explicit Naxx40TakeDryPathAction(PlayerbotAI* ai) : MovementAction(ai, TakeDryPath) {}

    bool isUseful() override { return CanTakeDryPath(*bot) && getMSTime() - _lastSearch >= SearchIntervalMs; }

    bool isPossible() override
    {
        return IsMovingAllowed() && !bot->GetVehicle() && !bot->GetTransport() && !bot->IsFalling() &&
               !bot->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE) && !bot->IsNonMeleeSpellCast(true);
    }

    bool Execute(Event /*event*/) override
    {
        _lastSearch = getMSTime();
        auto const& movement = *bot->movespline;
        G3D::Vector3 const destination = movement.FinalDestination();
        bool const wetDestination = TouchesSlime(*bot, destination);
        float const lookahead = std::min(movement.Velocity() * AlternateLookaheadSeconds, AlternateDestinationLimit);
        if (!wetDestination && AllDry(*bot, RemainingSpline(*bot, lookahead), lookahead))
            return false;

        float const limit = (destination - Location(*bot)).length() + AlternatePathAllowance;
        LastMovement& last = AI_VALUE(LastMovement&, "last movement");
        Movement::PointsArray path =
            wetDestination ? FindEquivalentPosition(destination, limit, last) : DryPathTo(*bot, destination, limit);
        if (!StartCheckedMovement(*bot, path))
            return false;

        RecordMovement(*bot, last, path.back(),
                       bot->IsInCombat() ? MovementPriority::MOVEMENT_COMBAT : MovementPriority::MOVEMENT_NORMAL);
        return true;
    }

private:
    Movement::PointsArray FindEquivalentPosition(G3D::Vector3 const& destination, float limit, LastMovement const& last)
    {
        MovementGeneratorType const movementType = bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
        if (!bot->IsInCombat() || botAI->IsHeal(bot) ||
            (movementType != CHASE_MOTION_TYPE && last.priority != MovementPriority::MOVEMENT_COMBAT))
            return {};

        Unit const* target = AI_VALUE(Unit*, "current target");
        if (!target || !target->IsAlive() || !bot->IsInMap(target))
            return {};

        float const range = (botAI->IsMelee(bot) ? sPlayerbotAIConfig.MeleeDistance : botAI->GetRange("spell")) +
                            bot->GetCombatReach() + target->GetCombatReach();
        if ((Location(*target) - destination).squaredLength() > range * range)
            return {};
        return FindDryCombatPosition(*bot, *target, destination, range, limit);
    }

    uint32 _lastSearch = 0;
};

class Naxx40SlimeStrategy final : public Strategy
{
public:
    explicit Naxx40SlimeStrategy(PlayerbotAI* ai) : Strategy(ai) {}

    std::string const getName() override { return Naxx40SlimeStrategyName; }

    void InitTriggers(std::vector<TriggerNode*>& triggers) override
    {
        Strategy::InitTriggers(triggers);
        triggers.push_back(new TriggerNode(InSlime, {NextAction(LeaveSlime, ACTION_EMERGENCY)}));
        triggers.push_back(new TriggerNode(MovingNearSlime, {NextAction(TakeDryPath, ACTION_RAID)}));
    }
};
}  // namespace

void AuthorizeNaxx40SlimeCrossing(PlayerbotAI& ai)
{
    RecordSlimeSpline(ai, AuthorizedCrossing);
}

bool IsNaxx40SlimeCrossingAuthorized(PlayerbotAI& ai)
{
    return IsSlimeSplineActive(ai, AuthorizedCrossing);
}

bool IsNaxx40SlimeEscapeActive(PlayerbotAI& ai)
{
    return IsSlimeSplineActive(ai, SlimeEscape);
}

Naxx40SlimeActionContext::Naxx40SlimeActionContext()
{
    creators[LeaveSlime] = [](PlayerbotAI* ai) { return new Naxx40LeaveSlimeAction(ai); };
    creators[TakeDryPath] = [](PlayerbotAI* ai) { return new Naxx40TakeDryPathAction(ai); };
}

Naxx40SlimeTriggerContext::Naxx40SlimeTriggerContext()
{
    creators[InSlime] = [](PlayerbotAI* ai) { return new Naxx40InSlimeTrigger(ai); };
    creators[MovingNearSlime] = [](PlayerbotAI* ai) { return new Naxx40MovingNearSlimeTrigger(ai); };
}

Naxx40SlimeStrategyContext::Naxx40SlimeStrategyContext()
{
    creators[Naxx40SlimeStrategyName] = [](PlayerbotAI* ai) { return new Naxx40SlimeStrategy(ai); };
}

Naxx40SlimeValueContext::Naxx40SlimeValueContext()
{
    creators[AuthorizedCrossing] = [](PlayerbotAI* ai)
    { return new ManualSetValue<SlimeMovement>(ai, {}, AuthorizedCrossing); };
    creators[SlimeEscape] = [](PlayerbotAI* ai)
    { return new ManualSetValue<SlimeMovement>(ai, {}, SlimeEscape); };
}
