/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FourHorsemen40.h"

#include "AttackAction.h"
#include "BossLookup.h"
#include "EncounterHelpers.h"
#include "GenericSpellActions.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Multiplier.h"
#include "Naxx40Encounter.h"
#include "ObjectAccessor.h"
#include "Playerbots.h"
#include "SpellMgr.h"
#include "Strategy.h"
#include "TankRoles.h"
#include "Timer.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <unordered_map>

namespace
{
constexpr char TacticsAction[] = "four horsemen 40 tactics";
constexpr char EncounterTrigger[] = "four horsemen 40 active";
constexpr uint32 Encounter = 12;
constexpr uint8 CornerCount = 4;
constexpr uint8 NoCorner = CornerCount;
constexpr uint32 RotationStacks = 2;
constexpr uint32 SnapshotInterval = 200;
constexpr uint32 RetryDelay = 5000;
constexpr uint32 HandoffWarningAfter = 10000;
constexpr float CornerArrival = 2.0f;
constexpr float BossCornerRange = 9.0f;
constexpr float AttackRange = 24.0f;
constexpr float TauntRange = 29.0f;

struct Horseman
{
    char const* name;
    uint32 data;
    uint32 entry;
    uint32 mark;
    Position corner;
};

// Individual Progression instance data keys and the final floor waypoints
// from the encounter script in the same Naxxramas WMO.
std::array<Horseman, CornerCount> const Horsemen = {{
    {"Korth'azz", 109, 351039, 28832, {2542.9f, -3015.0f, 241.35f}},
    {"Mograine", 106, 351037, 28834, {2583.9f, -2971.6f, 241.35f}},
    {"Blaumeux", 108, 351040, 28833, {2469.4f, -2947.6f, 241.28f}},
    {"Zeliek", 107, 351038, 28835, {2517.8f, -2896.6f, 241.28f}},
}};

std::array<Position, 2> const Handoffs = {{{2563.0f, -2993.0f, 241.41f}, {2494.0f, -2920.0f, 241.28f}}};
Position const RoomCenter(2540.0f, -2955.0f, 241.30f);
Position const WaitingPosition(2535.0f, -2941.0f, 241.48f);

using Bosses = std::array<Creature*, CornerCount>;

struct Member
{
    ObjectGuid guid;
    uint8 home;
};

struct Rotation
{
    bool flipped = false;
    bool handoff = false;
    bool warned = false;
    uint32 changedAt = 0;
};

struct RaidPlan
{
    ObjectGuid group;
    std::vector<Member> members;
    std::array<ObjectGuid, CornerCount> holders;
    std::array<Rotation, 2> rotations;
    uint32 sampledAt = 0;
    std::array<bool, CornerCount> reached = {};
};

struct Assignment
{
    uint8 corner = NoCorner;
    bool holder = false;
    bool handoff = false;
    ObjectGuid targetHolder;
};

std::mutex PlansMutex;
std::unordered_map<uint32, RaidPlan> Plans;

bool CanAssign(Player* member, Player const& anchor)
{
    return member && member->IsInWorld() && !member->IsDuringRemoveFromWorld() &&
        member->IsInMap(&anchor) && GET_PLAYERBOT_AI(member);
}

InstanceScript* ActiveEncounter(Player* bot)
{
    if (!IsNaxx40Map(bot->FindMap()) || !bot->GetGroup() || bot->GetExactDist2d(RoomCenter) > 110.0f)
        return nullptr;
    InstanceScript* instance = bot->GetInstanceScript();
    return instance && instance->GetBossState(Encounter) == IN_PROGRESS ? instance : nullptr;
}

Bosses ReadBosses(Player* bot, InstanceScript& instance)
{
    Bosses bosses = {};
    for (uint8 corner = 0; corner < CornerCount; ++corner)
    {
        Creature* boss = instance.GetCreature(Horsemen[corner].data);
        if (!boss || boss->GetEntry() != Horsemen[corner].entry)
            boss = RaidClear::InstanceCreature(bot, {Horsemen[corner].entry});
        if (boss && boss->IsInWorld() && boss->IsAlive())
            bosses[corner] = boss;
    }
    return bosses;
}

bool IsTank(Player* player)
{
    return PlayerbotAI::IsTank(player) || PlayerbotAI::IsExplicitMainTank(player);
}

bool CanHold(Player* player)
{
    if (PlayerbotAI::IsHeal(player))
        return false;
    switch (player->getClass())
    {
        case CLASS_WARRIOR:
        case CLASS_PALADIN:
        case CLASS_DRUID:
        case CLASS_DEATH_KNIGHT:
            return true;
        default:
            return false;
    }
}

Player* PlayerInRaid(Player& bot, ObjectGuid guid)
{
    Player* player = guid.IsEmpty() ? nullptr : ObjectAccessor::GetPlayer(bot, guid);
    return CanAssign(player, bot) && player->IsAlive() ? player : nullptr;
}

void SelectHolders(Player& bot, RaidPlan& plan)
{
    std::vector<Player*> candidates;
    for (Member const& member : plan.members)
        if (Player* player = PlayerInRaid(bot, member.guid); player && (IsTank(player) || CanHold(player)))
            candidates.push_back(player);

    Group* group = bot.GetGroup();
    std::sort(candidates.begin(), candidates.end(), [group](Player* lhs, Player* rhs)
    {
        auto priority = [group](Player* p)
        {
            return PlayerbotAI::IsMainTank(p) ? 0 : IsTank(p) ? (group->IsAssistant(p->GetGUID()) ? 1 : 2) : 3;
        };
        if (priority(lhs) != priority(rhs))
            return priority(lhs) < priority(rhs);
        if (lhs->GetMaxHealth() != rhs->GetMaxHealth())
            return lhs->GetMaxHealth() > rhs->GetMaxHealth();
        return lhs->GetGUID() < rhs->GetGUID();
    });

    for (uint8 home = 0; home < CornerCount; ++home)
    {
        if (PlayerInRaid(bot, plan.holders[home]))
            continue;
        ObjectGuid previous = plan.holders[home];
        plan.holders[home].Clear();
        for (Player* player : candidates)
        {
            if (std::find(plan.holders.begin(), plan.holders.end(), player->GetGUID()) != plan.holders.end())
                continue;
            plan.holders[home] = player->GetGUID();
            auto member = std::find_if(plan.members.begin(), plan.members.end(),
                [player](Member const& m) { return m.guid == player->GetGUID(); });
            member->home = home;
            if (!previous.IsEmpty())
                LOG_INFO("playerbots", "[Naxx40] Horsemen replacement at {}: {} ({})",
                    Horsemen[home].name, player->GetName(), IsTank(player) ? "tank" : "taunt-capable reserve");
            break;
        }
    }
}

void StartPlan(Player& bot, RaidPlan& plan)
{
    plan = {};
    Group* group = bot.GetGroup();
    plan.group = group->GetGUID();
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        if (Player* member = ref->GetSource(); CanAssign(member, bot))
            plan.members.push_back({member->GetGUID(), NoCorner});
        else if (member && !GET_PLAYERBOT_AI(member))
            LOG_INFO("playerbots", "[Naxx40] Horsemen manual player {} at ({:.1f}, {:.1f}, {:.1f}); outside the bot formation",
                member->GetName(), member->GetPositionX(), member->GetPositionY(), member->GetPositionZ());

    SelectHolders(bot, plan);
    std::array<uint8, 3> counts = {}; // healers, ranged, melee and spare tanks
    std::array<uint8, CornerCount> cornerSizes = {};
    for (Member& member : plan.members)
    {
        if (Player* player = ObjectAccessor::GetPlayer(bot, member.guid); player && member.home == NoCorner)
        {
            if (PlayerbotAI::IsHeal(player))
            {
                uint8 const index = counts[0]++;
                member.home = index < 2 ? 2 + index : index % 2;
            }
            else if (PlayerbotAI::IsRangedDps(player))
            {
                uint8 const index = counts[1]++;
                member.home = index < 4 ? 2 + (index % 2) : (index % 3 == 0);
            }
            else
                member.home = counts[2]++ % 3 == 2;
        }
        if (member.home < CornerCount)
            ++cornerSizes[member.home];
    }

    for (uint8 corner = 0; corner < CornerCount; ++corner)
    {
        Player* player = PlayerInRaid(bot, plan.holders[corner]);
        Position const& position = Horsemen[corner].corner;
        LOG_INFO("playerbots", "[Naxx40] Horsemen instance={} {} corner=({:.1f}, {:.1f}, {:.2f}) bots={} holder={} role={}",
            bot.GetInstanceId(), Horsemen[corner].name, position.GetPositionX(), position.GetPositionY(),
            position.GetPositionZ(), cornerSizes[corner], player ? player->GetName() : "none",
            player ? (IsTank(player) ? "tank" : "taunt-capable reserve") : "none");
    }
}

uint8 AssignedCorner(uint8 home, Rotation const& rotation)
{
    return rotation.flipped ? home ^ 1 : home;
}

bool OwnsTarget(Creature const* boss, ObjectGuid holder)
{
    return !boss || (!holder.IsEmpty() && boss->GetVictim() && boss->GetVictim()->GetGUID() == holder);
}

bool AtCorner(Creature const* boss, uint8 corner)
{
    return !boss || boss->GetExactDist(Horsemen[corner].corner) <= BossCornerRange;
}

void UpdateRotation(Player& bot, RaidPlan& plan, Bosses const& bosses, uint8 first, uint32 now)
{
    uint8 const second = first + 1;
    if (!bosses[first] && !bosses[second])
        return;

    Rotation& rotation = plan.rotations[first / 2];
    ObjectGuid const holderFirst = plan.holders[AssignedCorner(first, rotation)];
    ObjectGuid const holderSecond = plan.holders[AssignedCorner(second, rotation)];
    bool const owners = OwnsTarget(bosses[first], holderFirst) && OwnsTarget(bosses[second], holderSecond);
    if (rotation.handoff)
    {
        if (owners)
        {
            rotation.handoff = false;
            LOG_INFO("playerbots", "[Naxx40] Horsemen instance={} handoff {} / {} secured",
                bot.GetInstanceId(), Horsemen[first].name, Horsemen[second].name);
        }
        else if (!rotation.warned && now - rotation.changedAt >= HandoffWarningAfter)
        {
            rotation.warned = true;
            LOG_INFO("playerbots", "[Naxx40] Horsemen instance={} awaiting handoff {} / {} (victims {} / {})",
                bot.GetInstanceId(), Horsemen[first].name, Horsemen[second].name,
                bosses[first] && bosses[first]->GetVictim() ? bosses[first]->GetVictim()->GetName() : "none",
                bosses[second] && bosses[second]->GetVictim() ? bosses[second]->GetVictim()->GetName() : "none");
        }
        return;
    }

    Player* left = PlayerInRaid(bot, holderFirst);
    Player* right = PlayerInRaid(bot, holderSecond);
    if (!left || !right || !owners || now - rotation.changedAt < RetryDelay ||
        !AtCorner(bosses[first], first) || !AtCorner(bosses[second], second))
        return;

    uint32 const leftStacks = left->GetAuraCount(Horsemen[first].mark);
    uint32 const rightStacks = right->GetAuraCount(Horsemen[second].mark);
    bool const needSwap = (bosses[first] && leftStacks >= RotationStacks) ||
        (bosses[second] && rightStacks >= RotationStacks);
    // A newly entered corner may apply its mark on the way across the floor. Wait
    // for the mark of the corner being left to fall before reversing again.
    if (!needSwap || left->GetAuraCount(Horsemen[second].mark) >= RotationStacks ||
        right->GetAuraCount(Horsemen[first].mark) >= RotationStacks)
        return;

    rotation.flipped = !rotation.flipped;
    rotation.handoff = true;
    rotation.warned = false;
    rotation.changedAt = now;
    LOG_INFO("playerbots", "[Naxx40] Horsemen instance={} rotation {} / {} from actual stacks {}={} {}={}",
        bot.GetInstanceId(), Horsemen[first].name, Horsemen[second].name,
        left->GetName(), leftStacks, right->GetName(), rightStacks);
}

Assignment AssignmentFor(Player& bot, Bosses const& bosses)
{
    std::lock_guard<std::mutex> lock(PlansMutex);
    uint32 const now = getMSTime();
    RaidPlan& plan = Plans[bot.GetInstanceId()];
    if (plan.group != bot.GetGroup()->GetGUID() || now - plan.sampledAt > RetryDelay)
        StartPlan(bot, plan);

    if (!plan.sampledAt || now - plan.sampledAt >= SnapshotInterval)
    {
        plan.sampledAt = now;
        SelectHolders(bot, plan);
        UpdateRotation(bot, plan, bosses, 0, now);
        UpdateRotation(bot, plan, bosses, 2, now);
        for (uint8 corner = 0; corner < CornerCount; ++corner)
        {
            Creature* boss = bosses[corner];
            if (plan.reached[corner] || !boss || !AtCorner(boss, corner))
                continue;
            plan.reached[corner] = true;
            LOG_INFO("playerbots", "[Naxx40] Horsemen instance={} {} reached its corner at ({:.1f}, {:.1f}, {:.2f})",
                bot.GetInstanceId(), Horsemen[corner].name, boss->GetPositionX(), boss->GetPositionY(), boss->GetPositionZ());
        }
    }

    auto member = std::find_if(plan.members.begin(), plan.members.end(),
        [&bot](Member const& candidate) { return candidate.guid == bot.GetGUID(); });
    if (member == plan.members.end() || member->home == NoCorner)
        return {};

    Rotation const& rotation = plan.rotations[member->home / 2];
    return {AssignedCorner(member->home, rotation), plan.holders[member->home] == bot.GetGUID(), rotation.handoff,
            plan.holders[member->home]};
}

class FourHorsemen40Action final : public AttackAction
{
public:
    explicit FourHorsemen40Action(PlayerbotAI* ai) : AttackAction(ai, TacticsAction) {}

    bool Execute(Event /*event*/) override
    {
        InstanceScript* instance = ActiveEncounter(bot);
        if (!instance || !bot->IsAlive() || !bot->IsInCombat())
            return false;

        Bosses const bosses = ReadBosses(bot, *instance);
        Assignment const assignment = AssignmentFor(*bot, bosses);
        corner = assignment.corner;
        holder = assignment.holder;
        if (corner == NoCorner)
            return false;

        Creature* target = bosses[corner];
        bool const pairAlive = target || bosses[corner ^ 1];
        Position const& position = pairAlive ? Horsemen[corner].corner : WaitingPosition;
        fighting = holder || (target && !assignment.handoff && AtCorner(target, corner) && OwnsTarget(target, assignment.targetHolder));

        if (holder && target && target->GetVictim() != bot)
        {
            if (RaidClear::Tanks::Taunt(botAI, target))
                return true;

            if (bot->GetExactDist(target) > TauntRange || !bot->IsWithinLOSInMap(target))
            {
                if (assignment.handoff && bot->GetExactDist(Handoffs[corner / 2]) > CornerArrival)
                    return WalkTo(Handoffs[corner / 2]);
                return MoveNear(target, bot->IsWithinLOSInMap(target) ? TauntRange - 3.0f : 20.0f,
                    MovementPriority::MOVEMENT_COMBAT);
            }

            return AI_VALUE(Unit*, "current target") != target && Attack(target);
        }

        if (assignment.handoff && holder)
            return WalkTo(Handoffs[corner / 2]);

        if (!fighting || !target)
        {
            DropAttack();
            return WalkTo(position);
        }

        if (bot->GetExactDist(position) > CornerArrival)
            return WalkTo(position);
        if (PlayerbotAI::IsHeal(bot))
            return false;

        if (target->GetExactDist(bot) <= AttackRange && AI_VALUE(Unit*, "current target") != target)
            return Attack(target);
        return false;
    }

    bool AllowsOffense(Unit const* target) const
    {
        return corner != NoCorner && fighting && target && target->GetEntry() == Horsemen[corner].entry;
    }

private:
    bool WalkTo(Position const& position)
    {
        if (bot->GetExactDist(position) <= CornerArrival)
            return false;
        return MoveTo(533, position.GetPositionX(), position.GetPositionY(), position.GetPositionZ(),
            false, false, false, false, MovementPriority::MOVEMENT_COMBAT);
    }

    void DropAttack()
    {
        if (bot->GetVictim())
            bot->AttackStop();
        if (AI_VALUE(Unit*, "current target"))
            context->GetValue<Unit*>("current target")->Set(nullptr);
    }

    uint8 corner = NoCorner;
    bool holder = false;
    bool fighting = false;
};

class FourHorsemen40Trigger final : public Trigger
{
public:
    explicit FourHorsemen40Trigger(PlayerbotAI* ai) : Trigger(ai, EncounterTrigger) {}

    bool IsActive() override
    {
        if (ActiveEncounter(bot))
            return bot->IsAlive() && bot->IsInCombat();
        if (IsNaxx40Map(bot->FindMap()))
        {
            InstanceScript* instance = bot->GetInstanceScript();
            if (instance && instance->GetBossState(Encounter) != IN_PROGRESS)
            {
                std::lock_guard<std::mutex> lock(PlansMutex);
                Plans.erase(bot->GetInstanceId());
            }
        }
        return false;
    }
};

class FourHorsemen40Multiplier final : public Multiplier
{
public:
    explicit FourHorsemen40Multiplier(PlayerbotAI* ai) : Multiplier(ai, "four horsemen 40 control") {}

    float GetValue(Action* action) override
    {
        if (!ActiveEncounter(bot) || action->getName() == TacticsAction)
            return 1.0f;

        bool const melee = dynamic_cast<MeleeAction*>(action) != nullptr;
        if ((dynamic_cast<MovementAction*>(action) && !melee) ||
            RaidClear::Tanks::IsTauntAction(action) || EncounterHelpers::IsAoeThreatAction(bot, action))
            return 0.0f;
        if (dynamic_cast<CastHealingSpellAction*>(action))
            return 1.0f;

        Unit* target = action->GetTarget();
        auto* cast = dynamic_cast<CastSpellAction*>(action);
        SpellInfo const* spell = cast ?
            sSpellMgr->GetSpellInfo(AI_VALUE2(uint32, "spell id", cast->getSpell())) : nullptr;
        if (!melee && !(target && bot->IsValidAttackTarget(target)) && !(spell && !spell->IsPositive()))
            return 1.0f;

        auto* tactics = dynamic_cast<FourHorsemen40Action*>(context->GetAction(TacticsAction));
        if (!target || !bot->IsValidAttackTarget(target))
            target = AI_VALUE(Unit*, "current target");
        return tactics && tactics->AllowsOffense(target) ? 1.0f : 0.0f;
    }
};
}

FourHorsemen40ActionContext::FourHorsemen40ActionContext()
{
    creators[TacticsAction] = [](PlayerbotAI* ai) { return new FourHorsemen40Action(ai); };
}

FourHorsemen40TriggerContext::FourHorsemen40TriggerContext()
{
    creators[EncounterTrigger] = [](PlayerbotAI* ai) { return new FourHorsemen40Trigger(ai); };
}

void AddFourHorsemen40Triggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(EncounterTrigger, {NextAction(TacticsAction, ACTION_RAID + 4)}));
}

void AddFourHorsemen40Multipliers(PlayerbotAI* botAI, std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new FourHorsemen40Multiplier(botAI));
}
