/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Ground and air positioning adapted from electiveDev/playerbots-custom-strategies
 * by electiveDev, commit 58a5b7b5ed760d2236f894cf9b535d2c18e7bc69.
 */

#include "Sapphiron40.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "CellImpl.h"
#include "Creature.h"
#include "DKActions.h"
#include "DynamicObject.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "InstanceScript.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MovementActions.h"
#include "MoveSpline.h"
#include "Multiplier.h"
#include "Naxx40Encounter.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "SpellAuras.h"
#include "Strategy.h"
#include "Timer.h"
#include "Ai/Raid/RaidClear/Common/AggroLeash.h"

namespace
{
// Individual Progression's instance_naxxramas and boss_sapphiron_40.
constexpr uint32 DataSapphironBoss = 110;
constexpr uint32 SapphironEntry = 351018;
constexpr uint32 BlizzardEntry = 16474;
constexpr uint32 IceBlockEntry = 181247;
constexpr uint32 IceboltAura = 28522;
constexpr uint32 ChillAura = 28547;
constexpr uint32 SnapshotIntervalMs = 200;
constexpr uint32 OpeningHoldMs = 6500;
constexpr uint32 IncompleteVolleyMs = 12000;
constexpr uint32 IceboltTravelMarginMs = 1100;
constexpr size_t IceboltCount = 3;
constexpr float ArenaSearchRadius = 75.0f;
constexpr float BlizzardApproachRadius = 5.5f;
// Area checks include the target's combat reach, which is usually 1.5 yards.
constexpr float BlizzardEdgeMargin = 2.5f;
constexpr float GroundDangerRadius = 35.0f;
constexpr float OpeningAdvanceRadius = 38.0f;
constexpr float OpeningLateralClearance = 14.0f;
constexpr float GroundFlankStart = 94.0f;
constexpr float GroundFlankEnd = 116.0f;
constexpr float CoverWidth = 2.0f;
constexpr float CoverRange = 10.0f;
constexpr float Pi = 3.14159265f;

constexpr char GroundTrigger[] = "sapphiron 40 ground phase";
constexpr char FlightTrigger[] = "sapphiron 40 flight phase";
constexpr char BlizzardTrigger[] = "sapphiron 40 blizzard nearby";
constexpr char PositionAction[] = "sapphiron 40 position";

struct BlizzardZone
{
    Position center;
    float radius;
};

struct Observation
{
    uint32 instanceId = 0;
    ObjectGuid bossGuid;
    uint32 sampledAt = 0;
    uint32 engagedAt = 0;
    uint32 liftedAt = 0;
    uint32 finalBlockAt = 0;
    bool sheltering = false;
    std::vector<ObjectGuid> blocks;
    std::vector<BlizzardZone> blizzards;
};

Creature* FindSapphiron(Player* bot)
{
    if (!bot->IsAlive() || !bot->IsInCombat() || !IsNaxx40Map(bot->FindMap()))
        return nullptr;

    InstanceScript* instance = bot->GetInstanceScript();
    Creature* boss = instance ? instance->GetCreature(DataSapphironBoss) : nullptr;
    if (!boss || boss->GetEntry() != SapphironEntry || !boss->IsAlive() || !boss->IsInCombat() ||
        !boss->IsWithinDistInMap(bot, ArenaSearchRadius, false))
        return nullptr;

    return boss;
}

Observation const& Observe(Creature& boss)
{
    // A map update runs on one thread at a time. Cache GUIDs and positions, never world object pointers.
    thread_local Observation state;
    uint32 const now = getMSTime();
    if (state.instanceId != boss.GetInstanceId() || state.bossGuid != boss.GetGUID() || now - state.sampledAt > 5000)
    {
        state = Observation{};
        state.instanceId = boss.GetInstanceId();
        state.bossGuid = boss.GetGUID();
        state.engagedAt = now;
    }

    if (state.sampledAt && now - state.sampledAt < SnapshotIntervalMs)
        return state;

    state.sampledAt = now;
    state.blizzards.clear();
    std::list<Creature*> blizzards;
    boss.GetCreatureListWithEntryInGrid(blizzards, BlizzardEntry, ArenaSearchRadius);
    for (Creature const* blizzard : blizzards)
        if (blizzard->IsAlive() && blizzard->GetSummonerGUID() == boss.GetGUID())
            state.blizzards.push_back({blizzard->GetPosition(), BlizzardApproachRadius});

    // Each pulse leaves a patch at its cast position while the Blizzard creature keeps moving.
    std::list<WorldObject*> objects;
    Acore::AllWorldObjectsInRange check(&boss, ArenaSearchRadius);
    Acore::WorldObjectListSearcher<Acore::AllWorldObjectsInRange> searcher(
        &boss, objects, check, GRID_MAP_TYPE_MASK_DYNAMICOBJECT);
    Cell::VisitObjects(&boss, searcher, ArenaSearchRadius);
    for (WorldObject const* object : objects)
        if (DynamicObject const* patch = object->ToDynObject();
            patch && patch->GetSpellId() == ChillAura && patch->GetDuration() > 0 &&
            std::any_of(blizzards.begin(), blizzards.end(), [&boss, patch](Creature const* blizzard)
            {
                return blizzard->GetGUID() == patch->GetCasterGUID() && blizzard->GetSummonerGUID() == boss.GetGUID();
            }))
            state.blizzards.push_back({patch->GetPosition(), patch->GetRadius() + BlizzardEdgeMargin});

    state.blocks.clear();
    if (!boss.IsFlying())
    {
        state.liftedAt = 0;
        state.finalBlockAt = 0;
        state.sheltering = false;
        return state;
    }

    if (!state.liftedAt)
        state.liftedAt = now;

    for (auto const& ref : boss.GetMap()->GetPlayers())
    {
        Player const* member = ref.GetSource();
        if (member && member->IsAlive() && member->HasAura(IceboltAura, boss.GetGUID()))
            state.blocks.push_back(member->GetGUID());
    }
    std::sort(state.blocks.begin(), state.blocks.end());

    if (!state.sheltering && !state.blocks.empty())
    {
        // IP materializes one game object for each selected target. A projectile can still be in the air
        // briefly after the final object appears, so also look for the actual freeze.
        std::list<GameObject*> iceBlocks;
        boss.GetGameObjectListWithEntryInGrid(iceBlocks, IceBlockEntry, ArenaSearchRadius);
        size_t const materialized = std::count_if(iceBlocks.begin(), iceBlocks.end(), [&boss](GameObject const* block)
                                                  { return block->GetOwnerGUID() == boss.GetGUID(); });
        if (materialized >= IceboltCount && !state.finalBlockAt)
            state.finalBlockAt = now;

        state.sheltering = (state.finalBlockAt && (state.blocks.size() >= IceboltCount ||
                                                   now - state.finalBlockAt >= IceboltTravelMarginMs)) ||
                           (materialized && now - state.liftedAt >= IncompleteVolleyMs);
    }
    return state;
}

Position Offset(Position const& origin, float angle, float distance)
{
    return {origin.GetPositionX() + std::cos(angle) * distance, origin.GetPositionY() + std::sin(angle) * distance,
            origin.GetPositionZ()};
}

Position SpreadPosition(Creature const& boss, uint32 slot)
{
    // Stagger three rings to give all 40 slots room without putting healers and their targets at opposite walls.
    Position const& center = boss.GetHomePosition();
    if (slot < 8)
        return Offset(center, 2.0f * Pi * slot / 8.0f, 10.0f);
    if (slot < 22)
        return Offset(center, 2.0f * Pi * (slot - 8 + 0.5f) / 14.0f, 18.0f);
    return Offset(center, 2.0f * Pi * (slot - 22) / 18.0f, 26.0f);
}

bool NearBlizzard(Position const& position, Observation const& state)
{
    return std::any_of(state.blizzards.begin(), state.blizzards.end(), [&position](BlizzardZone const& blizzard)
                       { return position.GetExactDist2dSq(blizzard.center) < blizzard.radius * blizzard.radius; });
}

bool CanReachDry(PlayerbotAI* ai, Player& bot, Position const& destination);

std::optional<Position> EscapeBlizzard(PlayerbotAI* ai, Player& bot, Creature const& boss, Observation const& state)
{
    Position const* danger = nullptr;
    float minDistanceSq = 0.0f;
    for (BlizzardZone const& blizzard : state.blizzards)
    {
        float const distanceSq = bot.GetExactDist2dSq(blizzard.center);
        if (distanceSq < blizzard.radius * blizzard.radius && (!danger || distanceSq < minDistanceSq))
        {
            danger = &blizzard.center;
            minDistanceSq = distanceSq;
        }
    }

    Aura const* chill = bot.GetAura(ChillAura);
    if (!danger && chill && chill->GetType() == DYNOBJ_AURA_TYPE)
        if (DynamicObject const* patch = chill->GetDynobjOwner();
            bot.GetExactDist2d(patch) < patch->GetRadius() + BlizzardEdgeMargin)
            danger = patch;
    if (!danger)
        return std::nullopt;

    float const away = danger->GetAngle(&bot);
    for (float distance : {6.0f, 9.0f, 12.0f})
        for (float angleOffset : {0.0f, Pi / 4.0f, -Pi / 4.0f, Pi / 2.0f, -Pi / 2.0f, Pi})
        {
            Position destination = Offset(bot, away + angleOffset, distance);
            if (destination.GetExactDist2dSq(boss.GetHomePosition()) >= 42.0f * 42.0f ||
                destination.GetExactDist2dSq(danger) < BlizzardEdgeMargin * BlizzardEdgeMargin ||
                NearBlizzard(destination, state))
                continue;
            float const ground = bot.GetMapHeight(destination.GetPositionX(), destination.GetPositionY(),
                                                   destination.GetPositionZ() + 2.0f);
            if (ground <= INVALID_HEIGHT || std::abs(ground - destination.GetPositionZ()) > 3.0f)
                continue;
            destination.Relocate(destination.GetPositionX(), destination.GetPositionY(), ground);
            if (CanReachDry(ai, bot, destination))
                return destination;
        }
    return std::nullopt;
}

float FlankAngleDegrees(Creature const& boss, Position const& position)
{
    return std::remainder(boss.GetAngle(&position) - boss.GetOrientation(), 2.0f * Pi) * 180.0f / Pi;
}

bool InGroundFlank(Creature const& boss, Position const& position)
{
    // Cleave chains through the front half of the boss. Tail Sweep covers the rear 120 degrees.
    float const angle = std::abs(FlankAngleDegrees(boss, position));
    return angle >= GroundFlankStart && angle <= GroundFlankEnd;
}

bool CanReachDry(PlayerbotAI* ai, Player& bot, Position const& destination)
{
    PathGenerator path(&bot);
    constexpr uint32 IncompleteWalk = PATHFIND_INCOMPLETE | PATHFIND_NOPATH | PATHFIND_SHORTCUT |
                                      PATHFIND_NOT_USING_PATH | PATHFIND_FARFROMPOLY;
    if (!path.CalculatePath(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ()) ||
        !(path.GetPathType() & PATHFIND_NORMAL) || (path.GetPathType() & IncompleteWalk) ||
        path.getPathLength() > 35.0f)
        return false;

    auto const& end = path.GetActualEndPosition();
    if (destination.GetExactDistSq(end.x, end.y, end.z) > 1.0f ||
        RaidClear::Leash::WalkCheck(ai, &bot).Pulls(path.GetPath()))
        return false;

    auto const& points = path.GetPath();
    for (std::size_t i = 1; i < points.size(); ++i)
    {
        G3D::Vector3 const step = points[i] - points[i - 1];
        int const samples = std::max(1, static_cast<int>(std::ceil(step.length() / 2.0f)));
        for (int sample = 1; sample <= samples; ++sample)
        {
            G3D::Vector3 const point = points[i - 1] + step * (static_cast<float>(sample) / samples);
            PositionFullTerrainStatus terrain;
            bot.GetMap()->GetFullTerrainStatusForPosition(bot.GetPhaseMask(), point.x, point.y, point.z,
                                                          bot.GetCollisionHeight(), terrain);
            if (terrain.liquidInfo.Status & MAP_LIQUID_STATUS_IN_CONTACT)
                return false;
        }
    }
    return points.size() >= 2;
}

Player* GroundTank(Creature const& boss)
{
    Unit* victim = boss.GetVictim();
    Player* tank = victim ? victim->ToPlayer() : nullptr;
    return tank && tank->IsAlive() && (PlayerbotAI::IsTank(tank) || PlayerbotAI::IsExplicitMainTank(tank)) ? tank : nullptr;
}

bool HoldingOpening(Creature const& boss, Player const& bot, Observation const& state)
{
    Player const* tank = GroundTank(boss);
    return tank && tank != &bot && getMSTime() - state.engagedAt < OpeningHoldMs;
}

std::optional<Position> OpeningFlank(PlayerbotAI* ai, Player& bot, Creature const& boss, Observation const& state,
                                     uint32 slot)
{
    // The ranged pull starts Cleave's timer while the boss is still running at the tank.
    // Stay back or take a short step sideways, away from the tank's chain-Cleave targets.
    if (bot.GetExactDist2d(&boss) > OpeningAdvanceRadius)
        return std::nullopt;

    Player const* tank = GroundTank(boss);
    if (!tank)
        return std::nullopt;
    float const axis = boss.GetAngle(tank) + Pi / 2.0f;
    float const lateral = (bot.GetPositionX() - tank->GetPositionX()) * std::cos(axis) +
                          (bot.GetPositionY() - tank->GetPositionY()) * std::sin(axis);
    if (std::abs(lateral) >= OpeningLateralClearance)
        return std::nullopt;
    float const preferred = std::abs(lateral) < 1.0f ? (slot % 2 ? 1.0f : -1.0f) : (lateral < 0.0f ? -1.0f : 1.0f);
    for (float side : {preferred, -preferred})
    {
        float const distance = OpeningLateralClearance - lateral * side;
        if (distance > 18.0f)
            continue;
        Position destination = Offset(bot, axis + (side < 0.0f ? Pi : 0.0f), distance);
        float const ground = bot.GetMapHeight(destination.GetPositionX(), destination.GetPositionY(),
                                               destination.GetPositionZ() + 2.0f);
        if (ground <= INVALID_HEIGHT || std::abs(ground - destination.GetPositionZ()) > 3.0f)
            continue;
        destination.Relocate(destination.GetPositionX(), destination.GetPositionY(), ground);
        if (!NearBlizzard(destination, state) && CanReachDry(ai, bot, destination))
            return destination;
    }
    return std::nullopt;
}

bool HasCover(Player const& bot, Creature const& boss, Observation const& state)
{
    for (ObjectGuid const& guid : state.blocks)
        if (Player const* block = ObjectAccessor::GetPlayer(boss, guid))
            if (block->IsInBetween(&boss, &bot, CoverWidth) && block->IsWithinDist(&bot, CoverRange))
                return true;
    return false;
}

std::optional<Position> FindShelter(PlayerbotAI* ai, Player& bot, Creature const& boss, Observation const& state,
                                    uint32 slot)
{
    std::optional<Position> best;
    float bestCost = 0.0f;
    bool bestInBlizzard = true;
    uint32 const count = static_cast<uint32>(state.blocks.size());
    uint32 const seat = slot / count;
    float const depth = 3.0f + 1.1f * (seat % 5);
    float const lane = 1.1f * (static_cast<int32>((seat / 5) % 3) - 1);
    for (uint32 i = 0; i < count; ++i)
    {
        Player const* block = ObjectAccessor::GetPlayer(boss, state.blocks[i]);
        if (!block || boss.GetExactDist2dSq(block) < 0.25f)
            continue;

        float const away = boss.GetAngle(block);
        Position destination = Offset(Offset(*block, away, depth), away + Pi / 2.0f, lane);
        float const ground =
            bot.GetMapHeight(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ() + 2.0f);
        if (std::abs(ground - destination.GetPositionZ()) > 3.0f)
            continue;
        destination.Relocate(destination.GetPositionX(), destination.GetPositionY(), ground);

        // Prefer safe cover even if it uses another block. Slots spread when distances are close.
        bool const inBlizzard = NearBlizzard(destination, state);
        float const cost = bot.GetExactDist2d(destination) + (i == slot % count ? 0.0f : 4.0f);
        if ((!best || (bestInBlizzard && !inBlizzard) || (bestInBlizzard == inBlizzard && cost < bestCost)) &&
            CanReachDry(ai, bot, destination))
        {
            best = destination;
            bestCost = cost;
            bestInBlizzard = inBlizzard;
        }
    }
    return best;
}

class SapphironPositionAction final : public MovementAction
{
public:
    explicit SapphironPositionAction(PlayerbotAI* ai) : MovementAction(ai, PositionAction) {}

    bool isUseful() override
    {
        Creature* boss = FindSapphiron(bot);
        return boss && !bot->HasAura(IceboltAura, boss->GetGUID());
    }

    bool Execute(Event /*event*/) override
    {
        Creature* boss = FindSapphiron(bot);
        if (!boss || bot->HasAura(IceboltAura, boss->GetGUID()))
            return false;

        Observation const& state = Observe(*boss);
        uint32 const slot = static_cast<uint32>(std::clamp(botAI->GetGroupSlotIndex(bot), 0, 39));
        if (boss->IsFlying())
        {
            if (state.sheltering && !state.blocks.empty())
            {
                bool const covered = HasCover(*bot, *boss, state);
                if (covered && !NearBlizzard(*bot, state))
                    return StopCurrentMove();

                auto const destination = FindShelter(botAI, *bot, *boss, state, slot);
                if (destination && !NearBlizzard(*destination, state) &&
                    Move(*destination, MovementPriority::MOVEMENT_FORCED))
                    return true;
                // One pulse is survivable; don't walk into the open during Breath to avoid it.
                if (covered)
                    return StopCurrentMove();
                if (destination)
                    return Move(*destination, MovementPriority::MOVEMENT_FORCED);
            }

            if (auto const escape = EscapeBlizzard(botAI, *bot, *boss, state))
                return Move(*escape, MovementPriority::MOVEMENT_FORCED);

            Position const destination = SpreadPosition(*boss, slot);
            return bot->GetExactDist2dSq(destination) > 4.0f && !NearBlizzard(destination, state) &&
                   Move(destination, MovementPriority::MOVEMENT_COMBAT);
        }

        if (auto const escape = EscapeBlizzard(botAI, *bot, *boss, state))
            return Move(*escape, MovementPriority::MOVEMENT_FORCED);

        if (HoldingOpening(*boss, *bot, state))
        {
            auto const destination = OpeningFlank(botAI, *bot, *boss, state, slot);
            if (!destination)
                return StopCurrentMove();
            if (bot->isMoving() && !bot->movespline->Finalized())
            {
                G3D::Vector3 const end = bot->movespline->FinalDestination();
                if (destination->GetExactDistSq(end.x, end.y, end.z) <= 4.0f)
                    return true;
            }
            return Move(*destination, MovementPriority::MOVEMENT_COMBAT);
        }

        Player* tank = GroundTank(*boss);
        if (!tank || tank == bot ||
            !boss->IsWithinMeleeRange(tank, 2.0f) || bot->GetExactDist2d(boss) > GroundDangerRadius ||
            InGroundFlank(*boss, *bot))
            return false;

        if (bot->isMoving() && !bot->movespline->Finalized())
        {
            G3D::Vector3 const end = bot->movespline->FinalDestination();
            Position const destination(end.x, end.y, end.z);
            if (InGroundFlank(*boss, destination) && !NearBlizzard(destination, state))
                return true;
        }

        // Use the actual facing once Sapphiron has reached the tank. Prefer the nearer flank,
        // then try an alternate offset before crossing to the far side.
        float const side = FlankAngleDegrees(*boss, *bot) < 0.0f ? -1.0f : 1.0f;
        bool const healer = PlayerbotAI::IsHeal(bot);
        bool const ranged = healer || PlayerbotAI::IsRanged(bot);
        float const radius = ranged ? (healer ? 18.0f : 20.0f) + (slot % 3) * 2.0f : 6.0f;
        float const offset = static_cast<float>(static_cast<int32>((slot / 3) % 7) - 3);
        for (float direction : {side, -side})
        {
            for (float angle : {105.0f + 2.0f * offset, 113.0f, 97.0f})
            {
                Position destination = Offset(*boss, boss->GetOrientation() + direction * angle * Pi / 180.0f, radius);
                float const ground = bot->GetMapHeight(destination.GetPositionX(), destination.GetPositionY(),
                                                       destination.GetPositionZ() + 2.0f);
                if (ground <= INVALID_HEIGHT || std::abs(ground - destination.GetPositionZ()) > 3.0f)
                    continue;
                destination.Relocate(destination.GetPositionX(), destination.GetPositionY(), ground);
                if (!NearBlizzard(destination, state) && CanReachDry(botAI, *bot, destination) &&
                    Move(destination, MovementPriority::MOVEMENT_COMBAT))
                    return true;
            }
        }
        return false;
    }

private:
    bool StopCurrentMove()
    {
        if (!bot->isMoving())
            return false;

        bot->GetMotionMaster()->Clear();
        bot->StopMoving();
        AI_VALUE(LastMovement&, "last movement").clear();
        return true;
    }

    bool Move(Position const& destination, MovementPriority priority)
    {
        return MoveTo(bot->GetMapId(), destination.GetPositionX(), destination.GetPositionY(),
                      destination.GetPositionZ(), false, false, false, false, priority, true);
    }
};

class SapphironPhaseTrigger final : public Trigger
{
public:
    SapphironPhaseTrigger(PlayerbotAI* ai, char const* name, bool flight) : Trigger(ai, name), flight(flight) {}

    bool IsActive() override
    {
        Creature const* boss = FindSapphiron(bot);
        return boss && boss->IsFlying() == flight && !bot->HasAura(IceboltAura, boss->GetGUID());
    }

private:
    bool flight;
};

class SapphironBlizzardTrigger final : public Trigger
{
public:
    explicit SapphironBlizzardTrigger(PlayerbotAI* ai) : Trigger(ai, BlizzardTrigger) {}

    bool IsActive() override
    {
        Creature* boss = FindSapphiron(bot);
        return boss && !bot->HasAura(IceboltAura, boss->GetGUID()) && NearBlizzard(*bot, Observe(*boss));
    }
};

class SapphironPositionMultiplier final : public Multiplier
{
public:
    explicit SapphironPositionMultiplier(PlayerbotAI* ai) : Multiplier(ai, "sapphiron 40 positioning") {}

    float GetValue(Action* action) override
    {
        Creature* boss = FindSapphiron(bot);
        if (!boss || dynamic_cast<SapphironPositionAction*>(action))
            return 1.0f;

        if (dynamic_cast<CombatFormationMoveAction*>(action) || dynamic_cast<CastDeathGripAction*>(action) ||
            (dynamic_cast<MovementAction*>(action) && (boss->IsFlying() || HoldingOpening(*boss, *bot, Observe(*boss)))))
            return 0.0f;

        return 1.0f;
    }
};
}  // namespace

Sapphiron40ActionContext::Sapphiron40ActionContext()
{
    creators[PositionAction] = [](PlayerbotAI* ai) { return new SapphironPositionAction(ai); };
}

Sapphiron40TriggerContext::Sapphiron40TriggerContext()
{
    creators[GroundTrigger] = [](PlayerbotAI* ai) { return new SapphironPhaseTrigger(ai, GroundTrigger, false); };
    creators[FlightTrigger] = [](PlayerbotAI* ai) { return new SapphironPhaseTrigger(ai, FlightTrigger, true); };
    creators[BlizzardTrigger] = [](PlayerbotAI* ai) { return new SapphironBlizzardTrigger(ai); };
}

void AddSapphiron40Triggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(GroundTrigger, {NextAction(PositionAction, ACTION_RAID + 2)}));
    triggers.push_back(new TriggerNode(FlightTrigger, {NextAction(PositionAction, ACTION_EMERGENCY + 1)}));
    triggers.push_back(new TriggerNode(BlizzardTrigger, {NextAction(PositionAction, ACTION_EMERGENCY + 2)}));
}

void AddSapphiron40Multipliers(PlayerbotAI* botAI, std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new SapphironPositionMultiplier(botAI));
}
