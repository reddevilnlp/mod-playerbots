/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "FourHorsemen40.h"
#include "FourHorsemen40Hazards.h"

#include "AttackAction.h"
#include "BossLookup.h"
#include "DruidShapeshiftActions.h"
#include "EncounterHelpers.h"
#include "GenericSpellActions.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "MoveSpline.h"
#include "Multiplier.h"
#include "Naxx40Encounter.h"
#include "ObjectAccessor.h"
#include "Playerbots.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "Strategy.h"
#include "TankRoles.h"
#include "Timer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace
{
constexpr char TacticsAction[] = "four horsemen 40 tactics";
constexpr char EncounterTrigger[] = "four horsemen 40 active";
constexpr uint32 Encounter = 12;
constexpr uint8 CornerCount = 4;
constexpr uint8 NoCorner = CornerCount;
constexpr uint8 NoSupportSlot = 5;
constexpr uint32 RotationStacks = 2;
constexpr uint32 SnapshotInterval = 200;
constexpr uint32 RetryDelay = 5000;
constexpr uint32 HandoffWarningAfter = 10000;
constexpr float CornerArrival = 2.0f;
constexpr float BossCornerRange = 9.0f;
constexpr float AttackRange = 28.0f;
constexpr float TauntRange = 29.0f;
constexpr float PlatformExitZ = 243.5f;
constexpr float VoidEscapeCornerRadius = 6.0f;
constexpr float RearCornerRange = 14.0f;
constexpr float RearSupportLocalRange = 28.0f;
constexpr float HealerCoverageRange = 35.0f;
constexpr float HealerCoverageMana = 10.0f;
constexpr float SupportTransferRange = 40.0f;
// The two rear marks have a 45-yard radius. Leave room for combat reach.
constexpr float RearMarkClearance = 48.0f;
constexpr uint32 TauntSpell = 355;
constexpr uint32 DefensiveStance = 71;
constexpr uint32 BattleStance = 2457;
constexpr uint32 BerserkerStance = 2458;
constexpr uint32 Growl = 6795;
constexpr uint32 BearForm = 5487;
constexpr uint32 DireBearForm = 9634;
constexpr uint32 HandOfReckoning = 62124;
constexpr uint32 RighteousFury = 25780;
constexpr uint32 DarkCommand = 56222;
constexpr uint32 FrostPresence = 48263;
constexpr uint32 BloodPresence = 48266;
constexpr uint32 UnholyPresence = 48265;

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
// Dry MMAP approaches from the rear exchange point. Try the side that walks
// around a pool at the nominal corner before the outward pads.
std::array<std::array<Position, 6>, 2> const RearHolderPads = {{
    {{{2460.92f, -2947.82f, 241.28f}, {2466.30f, -2950.86f, 241.28f}, {2465.27f, -2951.95f, 241.28f},
      {2463.55f, -2953.77f, 241.28f}, {2461.14f, -2956.30f, 241.28f}, {2469.62f, -2956.08f, 241.28f}}},
    {{{2517.95f, -2888.62f, 241.28f}, {2526.28f, -2896.38f, 241.28f}, {2520.90f, -2893.34f, 241.28f},
      {2521.93f, -2892.25f, 241.28f}, {2523.65f, -2890.43f, 241.28f}, {2525.99f, -2888.06f, 241.28f}}},
}};
std::array<uint32, CornerCount> constexpr MarkSpirits = {16778, 16775, 16776, 16777};
// The four starting spots are 19 yards from the holder. Use the last slot
// only for a reinforcement after filling the established formation.
std::array<std::array<Position, NoSupportSlot>, 2> const RearSupport = {{
    {{{2459.90f, -2931.15f, 241.28f}, {2469.40f, -2966.60f, 241.28f},
      {2488.40f, -2947.60f, 241.28f}, {2478.90f, -2931.15f, 241.28f}, {2478.90f, -2964.05f, 241.28f}}},
    {{{2501.35f, -2887.10f, 241.28f}, {2536.80f, -2896.60f, 241.28f},
      {2501.35f, -2906.10f, 241.28f}, {2527.30f, -2913.05f, 241.28f}, {2511.85f, -2918.82f, 241.28f}}},
}};
Position const RoomCenter(2540.0f, -2955.0f, 241.30f);
Position const WaitingPosition(2535.0f, -2941.0f, 241.48f);
Position const OpeningCamp(2555.9f, -2996.4f, 241.4f);

using Bosses = std::array<Creature*, CornerCount>;

struct Member
{
    ObjectGuid guid;
    uint8 home;
    uint8 supportSlot = NoSupportSlot;
    uint8 assignedCorner = NoCorner;
    uint8 assignedSlot = NoSupportSlot;
    uint8 clearingMarks = 0;
};

struct Rotation
{
    bool flipped = false;
    bool handoff = false;
    bool warned = false;
    uint32 changedAt = 0;
    std::array<bool, 2> rearSettled = {};
    std::array<bool, 2> rearDecayed = {};
    bool rearWarned = false;
};

struct RaidPlan
{
    ObjectGuid group;
    std::vector<Member> members;
    std::array<ObjectGuid, CornerCount> holders;
    std::array<Rotation, 2> rotations;
    uint32 sampledAt = 0;
    std::array<bool, CornerCount> reached = {};
    bool rearReleased = false;
};

struct Assignment
{
    uint8 corner = NoCorner;
    uint8 supportSlot = NoSupportSlot;
    bool holder = false;
    bool handoff = false;
    bool opening = false;
    ObjectGuid targetHolder;
    bool standby = false;
    bool redirected = false;
    uint8 clearingMarks = 0;
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
            return player->HasSpell(TauntSpell) && player->HasSpell(DefensiveStance);
        case CLASS_PALADIN:
            return player->HasSpell(HandOfReckoning);
        case CLASS_DRUID:
            return player->HasSpell(Growl) && (player->HasSpell(DireBearForm) || player->HasSpell(BearForm));
        case CLASS_DEATH_KNIGHT:
            return player->HasSpell(DarkCommand);
        default:
            return false;
    }
}

uint32 RequiredHolderBuff(Player const& player)
{
    ShapeshiftForm const form = player.GetShapeshiftForm();
    switch (player.getClass())
    {
        case CLASS_WARRIOR:
            return form == FORM_DEFENSIVESTANCE ? 0 : DefensiveStance;
        case CLASS_DRUID:
            if (form == FORM_BEAR || form == FORM_DIREBEAR)
                return 0;
            return player.HasSpell(DireBearForm) ? DireBearForm : BearForm;
        case CLASS_PALADIN:
            return player.HasSpell(RighteousFury) && !player.HasAura(RighteousFury) ? RighteousFury : 0;
        case CLASS_DEATH_KNIGHT:
            return player.HasSpell(FrostPresence) && !player.HasAura(FrostPresence) ? FrostPresence : 0;
        default:
            return 0;
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
        if (Player* player = PlayerInRaid(bot, member.guid); player && CanHold(player))
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
        if (!IsTank(lhs) && !IsTank(rhs))
        {
            auto reservePriority = [](Player* p)
            {
                // Bear form supplies armor and stamina even when a melee reserve has no shield.
                return (IsSelfBot(p) ? 2 : 0) + (p->getClass() == CLASS_DRUID ? 0 : 1);
            };
            if (reservePriority(lhs) != reservePriority(rhs))
                return reservePriority(lhs) < reservePriority(rhs);
        }
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
                member.home = index < 4 ? 2 + (index % 2) : index % 2;
                if (member.home >= 2)
                    member.supportSlot = index / 2;
            }
            else if (PlayerbotAI::IsRangedDps(player))
            {
                uint8 const index = counts[1]++;
                member.home = index < 4 ? 2 + (index % 2) : (index % 3 == 0);
                if (member.home >= 2)
                    member.supportSlot = 2 + index / 2;
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

Creature const* MarkCaster(Player* bot, Bosses const& bosses, uint8 corner)
{
    return bosses[corner] ? bosses[corner] : RaidClear::InstanceCreature(bot, {MarkSpirits[corner]});
}

float MarkRadius(uint8 corner)
{
    return corner < 2 ? 55.0f : 45.0f;
}

void AllocateSupport(Player& bot, RaidPlan& plan, Bosses const& bosses)
{
    struct Candidate
    {
        Member& member;
        Player* player;
        uint8 homeCorner;
        uint8 corner = NoCorner;
        uint8 slot = NoSupportSlot;
        bool healer = false;
        uint8 marks = 0;
        bool leaveForMarks = false;
    };
    std::vector<Candidate> support;
    support.reserve(plan.members.size());
    std::array<uint8, CornerCount> healerCount = {};
    std::array<std::array<uint8, NoSupportSlot>, CornerCount> occupiedSlots = {};
    std::array<bool, CornerCount> liveHolder = {};
    std::array<Player*, CornerCount> tanks = {};
    std::array<Creature const*, CornerCount> casters = {};
    for (uint8 corner = 0; corner < CornerCount; ++corner)
    {
        if (bosses[corner])
            tanks[corner] = PlayerInRaid(bot, plan.holders[AssignedCorner(corner, plan.rotations[corner / 2])]);
        liveHolder[corner] = tanks[corner] != nullptr;
        casters[corner] = MarkCaster(&bot, bosses, corner);
    }

    auto release = [&](Candidate& candidate)
    {
        if (candidate.corner != NoCorner)
        {
            healerCount[candidate.corner] -= candidate.healer;
            if (candidate.slot < NoSupportSlot)
                --occupiedSlots[candidate.corner][candidate.slot];
        }
        candidate.corner = NoCorner;
        candidate.slot = NoSupportSlot;
    };
    auto occupy = [&](Candidate& candidate, uint8 corner, uint8 slot)
    {
        release(candidate);
        candidate.corner = corner;
        candidate.slot = slot;
        healerCount[corner] += candidate.healer;
        if (slot < NoSupportSlot)
            ++occupiedSlots[corner][slot];
    };

    for (Member& member : plan.members)
    {
        Player* player = PlayerInRaid(bot, member.guid);
        if (!player || member.home == NoCorner || plan.holders[member.home] == member.guid)
            continue;

        uint8 const corner = AssignedCorner(member.home, plan.rotations[member.home / 2]);
        Candidate& candidate = support.emplace_back(Candidate{member, player, corner});
        candidate.healer = PlayerbotAI::IsHeal(player);
        if (bosses[corner] && (corner < 2 || (member.supportSlot < NoSupportSlot &&
            !occupiedSlots[corner][member.supportSlot])))
            occupy(candidate, corner, member.supportSlot);
    }

    auto freeSlot = [&](uint8 corner, bool healer)
    {
        if (corner >= 2)
            for (uint8 slot = healer ? 0 : 2; slot < (healer ? 2 : NoSupportSlot); ++slot)
                if (!occupiedSlots[corner][slot])
                    return slot;
        return NoSupportSlot;
    };

    // First cover every living holder. A healer already at a living corner can
    // move only if their group has a second healer.
    for (uint8 minimum : std::array<uint8, 2>{1, 2})
        for (uint8 corner = 0; corner < CornerCount; ++corner)
        {
            if (!liveHolder[corner] || healerCount[corner] >= minimum)
                continue;
            uint8 const slot = freeSlot(corner, true);
            if (corner >= 2 && slot == NoSupportSlot)
                continue;

            Candidate* choice = nullptr;
            auto priority = [corner](Candidate const& candidate)
            {
                return (candidate.corner != NoCorner ? 2 : 0) + (candidate.member.assignedCorner != corner);
            };
            for (Candidate& candidate : support)
                if (candidate.healer && candidate.corner != corner &&
                    (candidate.corner == NoCorner || (minimum == 1 && healerCount[candidate.corner] > 1)) &&
                    (!choice || priority(candidate) < priority(*choice)))
                    choice = &candidate;
            if (choice)
                occupy(*choice, corner, slot);
        }

    uint8 sharedBlaumeuxRanged = 0;
    for (Candidate& candidate : support)
    {
        if (candidate.healer || candidate.corner != NoCorner)
            continue;

        uint8 const opposite = candidate.homeCorner ^ 1;
        if (opposite < 2 && liveHolder[opposite])
            occupy(candidate, opposite, NoSupportSlot);
        else if (liveHolder[0] || liveHolder[1])
            occupy(candidate, liveHolder[0] ? 0 : 1, NoSupportSlot);
        else if (PlayerbotAI::IsRangedDps(candidate.player))
        {
            for (uint8 corner : std::array<uint8, 2>{2, 3})
                if (liveHolder[corner])
                    if (uint8 const slot = freeSlot(corner, false); slot != NoSupportSlot)
                    {
                        occupy(candidate, corner, slot);
                        break;
                    }
            // Blaumeux can take a full ranged group. Reserve the individual
            // spacing restriction for Zeliek, where Holy Wrath bounces.
            if (candidate.corner == NoCorner && liveHolder[2])
                occupy(candidate, 2, 2 + sharedBlaumeuxRanged++ % (NoSupportSlot - 2));
        }
        else if (liveHolder[2])
            occupy(candidate, 2, NoSupportSlot);
    }

    auto isWithinMark = [&](Position const& position, uint8 mark)
    {
        return casters[mark] && position.GetExactDist(casters[mark]) < MarkRadius(mark) + 3.0f;
    };
    auto supportPosition = [](uint8 corner, uint8 slot) -> Position const&
    {
        return corner >= 2 && slot < NoSupportSlot ? RearSupport[corner - 2][slot] : Horsemen[corner].corner;
    };
    auto clearsMarks = [&](Position const& position, uint8 marks)
    {
        for (uint8 mark = 0; mark < CornerCount; ++mark)
            if ((marks & (1u << mark)) && isWithinMark(position, mark))
                return false;
        return true;
    };

    for (Candidate& candidate : support)
    {
        Position const& destination = candidate.corner == NoCorner ? candidate.player->GetPosition() :
            supportPosition(candidate.corner, candidate.slot);
        uint8 doubled = 0;
        for (uint8 mark = 0; mark < CornerCount; ++mark)
        {
            uint32 const stacks = candidate.player->GetAuraCount(Horsemen[mark].mark);
            if (stacks && (candidate.member.clearingMarks & (1u << mark)))
            {
                candidate.marks |= 1u << mark;
                candidate.leaveForMarks = true;
            }
            if (stacks < RotationStacks || (!isWithinMark(destination, mark) && !isWithinMark(*candidate.player, mark)))
                continue;
            candidate.marks |= 1u << mark;
            ++doubled;
            candidate.leaveForMarks |= stacks > RotationStacks;
        }
        candidate.leaveForMarks |= doubled >= 2;
    }

    auto coveringHealer = [&](uint8 corner, Candidate const& leaving) -> Candidate const*
    {
        if (corner == NoCorner || !tanks[corner])
            return nullptr;
        auto const witness = std::find_if(support.begin(), support.end(), [&](Candidate const& witness)
        {
            return &witness != &leaving && witness.healer && witness.corner == corner &&
                (!witness.member.clearingMarks || witness.member.assignedCorner == corner) &&
                witness.player->GetPowerPct(POWER_MANA) >= HealerCoverageMana &&
                !witness.player->HasUnitFlag(UNIT_FLAG_SILENCED) &&
                witness.player->IsWithinDistInMap(tanks[corner], HealerCoverageRange) &&
                witness.player->IsWithinLOSInMap(tanks[corner]);
        });
        return witness == support.end() ? nullptr : &*witness;
    };
    auto covered = [&](uint8 corner, Candidate const& leaving)
    {
        return corner == NoCorner || !tanks[corner] || coveringHealer(corner, leaving);
    };

    for (Candidate& candidate : support)
    {
        if (!candidate.marks)
            continue;
        uint8 const from = candidate.corner;
        if (candidate.healer && !candidate.member.clearingMarks && from != NoCorner && !covered(from, candidate))
        {
            // Prepare a spare at two stacks. No marked healer leaves a tank
            // until someone assigned to that tank can actually heal it.
            uint8 slot = freeSlot(from, true);
            if (from >= 2 && slot == NoSupportSlot)
                slot = freeSlot(from, false);
            if (from < 2 || slot != NoSupportSlot)
            {
                Candidate* spare = nullptr;
                auto priority = [from](Candidate const& p)
                {
                    return (p.corner != NoCorner ? 2 : 0) + (p.member.assignedCorner != from);
                };
                for (Candidate& donor : support)
                    if (donor.healer && donor.corner != from && !donor.marks &&
                        donor.player->GetPowerPct(POWER_MANA) >= HealerCoverageMana &&
                        donor.player->GetAuraCount(Horsemen[from].mark) < RotationStacks &&
                        covered(donor.corner, donor) && (!spare || priority(donor) < priority(*spare)))
                        spare = &donor;
                if (spare)
                    occupy(*spare, from, slot);
            }
            if (!covered(from, candidate))
            {
                candidate.leaveForMarks = false;
                continue;
            }
        }

        if (!candidate.leaveForMarks || (from != NoCorner &&
            clearsMarks(supportPosition(from, candidate.slot), candidate.marks)))
            continue;

        if (candidate.healer && !candidate.member.clearingMarks)
            if (Candidate const* cover = coveringHealer(from, candidate))
                LOG_INFO("playerbots", "[Naxx40] Horsemen instance={} healer {} authorized to rotate from {} after {} covers {}",
                    bot.GetInstanceId(), candidate.player->GetName(), Horsemen[from].name, cover->player->GetName(),
                    tanks[from]->GetName());

        // Try the prior safe assignment, the other side of this pair, then
        // the remaining living corners. Rear destinations consume a unique slot.
        uint8 const origin = from == NoCorner ? candidate.homeCorner : from;
        std::array<uint8, 6> const order = {candidate.member.assignedCorner, uint8(origin ^ 1), 0, 1, 2, 3};
        bool moved = false;
        for (uint8 to : order)
        {
            if (to == from || to == NoCorner || !liveHolder[to] || plan.rotations[to / 2].handoff ||
                candidate.player->GetAuraCount(Horsemen[to].mark) >= RotationStacks)
                continue;
            if (to >= 2 && !candidate.healer && !PlayerbotAI::IsRangedDps(candidate.player))
                continue;
            uint8 const slot = freeSlot(to, candidate.healer);
            Position const& destination = supportPosition(to, slot);
            if ((to >= 2 && slot == NoSupportSlot) ||
                candidate.player->GetExactDist(destination) > SupportTransferRange ||
                !clearsMarks(destination, candidate.marks))
                continue;
            occupy(candidate, to, slot);
            moved = true;
            break;
        }
        if (!moved)
            release(candidate);
    }

    for (Candidate const& candidate : support)
    {
        Member& member = candidate.member;
        if (candidate.corner != NoCorner && candidate.corner != candidate.homeCorner &&
            (member.assignedCorner != candidate.corner || member.assignedSlot != candidate.slot))
            LOG_INFO("playerbots", "[Naxx40] Horsemen instance={} support reassigned {} ({}) {} -> {}{}",
                bot.GetInstanceId(), candidate.player->GetName(), candidate.healer ? "healer" : "dps",
                Horsemen[candidate.homeCorner].name, Horsemen[candidate.corner].name,
                candidate.leaveForMarks ? " for individual marks" :
                    candidate.healer ? " for tank coverage" : " after losing the assigned target");
        uint8 const clearing = candidate.leaveForMarks ? candidate.marks : 0;
        if (clearing != member.clearingMarks)
            LOG_INFO("playerbots", "[Naxx40] Horsemen instance={} {} individual marks mask={} -> {}",
                bot.GetInstanceId(), candidate.player->GetName(), clearing,
                candidate.corner == NoCorner ? "mark retreat" : Horsemen[candidate.corner].name);
        member.clearingMarks = clearing;
        member.assignedCorner = candidate.corner;
        member.assignedSlot = candidate.slot;
    }
}

bool OwnsTarget(Creature const* boss, ObjectGuid holder)
{
    return !boss || (!holder.IsEmpty() && boss->GetVictim() && boss->GetVictim()->GetGUID() == holder);
}

bool AtCorner(Creature const* boss, uint8 corner)
{
    return !boss || boss->GetExactDist(Horsemen[corner].corner) <= (corner >= 2 ? RearCornerRange : BossCornerRange);
}

Creature const* RearMarkSource(Player* bot, Bosses const& bosses, uint8 corner)
{
    return MarkCaster(bot, bosses, corner);
}

float MarkDistance(Position const& position, Creature const* source)
{
    return source ? position.GetExactDist(source) : -1.0f;
}

bool OutsideRearMark(Position const& position, Creature const* source)
{
    return !source || MarkDistance(position, source) >= RearMarkClearance;
}

void LogRearHolder(Player& tank, Creature const* assigned, Creature const* oldSource, uint8 corner, char const* stage)
{
    Position const bossPosition = assigned ? assigned->GetPosition() : Horsemen[corner].corner;
    Aura const* oldMark = tank.GetAura(Horsemen[corner ^ 1].mark);
    LOG_INFO("playerbots", "[Naxx40] Horsemen rear instance={} {} {} -> {}{} tank=({:.1f}, {:.1f}, {:.2f}) "
        "boss=({:.1f}, {:.1f}, {:.2f}) oldDist={:.1f}yd oldStacks={} oldMs={}",
        tank.GetInstanceId(), stage, tank.GetName(), Horsemen[corner].name, assigned ? "" : " (dead corner)",
        tank.GetPositionX(), tank.GetPositionY(), tank.GetPositionZ(), bossPosition.GetPositionX(), bossPosition.GetPositionY(),
        bossPosition.GetPositionZ(), MarkDistance(tank, oldSource), oldMark ? oldMark->GetStackAmount() : 0,
        oldMark ? oldMark->GetDuration() : 0);
}

bool MeasureRearRotation(Player& bot, Bosses const& bosses, Rotation& rotation,
                         std::array<Player*, 2> const& tanks, uint32 now)
{
    bool bothSettled = true;
    bool const warn = rotation.changedAt && !rotation.rearWarned && now - rotation.changedAt >= 16000;
    for (uint8 index = 0; index < tanks.size(); ++index)
    {
        uint8 const corner = 2 + index;
        Player* tank = tanks[index];
        if (!tank)
        {
            bothSettled = false;
            continue;
        }

        Creature const* oldSource = RearMarkSource(&bot, bosses, corner ^ 1);
        Creature const* assigned = bosses[corner];
        bool const settled = AtCorner(assigned, corner) && tank->GetExactDist(Horsemen[corner].corner) <= RearCornerRange &&
            OwnsTarget(assigned, tank->GetGUID()) && OutsideRearMark(*tank, oldSource);
        bothSettled &= settled;
        if (rotation.changedAt && settled && !rotation.rearSettled[index])
        {
            rotation.rearSettled[index] = true;
            LogRearHolder(*tank, assigned, oldSource, corner, "corner re-entry");
        }
        if (rotation.rearSettled[index] && !rotation.rearDecayed[index] && !tank->HasAura(Horsemen[corner ^ 1].mark))
        {
            rotation.rearDecayed[index] = true;
            LogRearHolder(*tank, assigned, oldSource, corner, "old mark expired");
        }
        if (warn && !settled)
            LogRearHolder(*tank, assigned, oldSource, corner, "corner re-entry pending");
    }
    rotation.rearWarned |= warn;
    return bothSettled;
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
    Player* left = PlayerInRaid(bot, holderFirst);
    Player* right = PlayerInRaid(bot, holderSecond);
    if (rotation.handoff)
    {
        if (owners)
        {
            rotation.handoff = false;
            LOG_INFO("playerbots", "[Naxx40] Horsemen instance={} handoff {} / {} secured",
                bot.GetInstanceId(), Horsemen[first].name, Horsemen[second].name);
            if (first == 2)
            {
                if (left)
                    LogRearHolder(*left, bosses[first], RearMarkSource(&bot, bosses, second), first, "handoff");
                if (right)
                    LogRearHolder(*right, bosses[second], RearMarkSource(&bot, bosses, first), second, "handoff");
            }
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

    bool const settled = first == 2 ? MeasureRearRotation(bot, bosses, rotation, {left, right}, now) :
        AtCorner(bosses[first], first) && AtCorner(bosses[second], second);
    if (!left || !right || !owners || !settled || now - rotation.changedAt < RetryDelay)
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
    rotation.rearSettled = {};
    rotation.rearDecayed = {};
    rotation.rearWarned = false;
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
        if (!plan.rearReleased)
        {
            bool ownedBelowPlatform = true;
            for (uint8 i = 0; i < CornerCount; ++i)
                ownedBelowPlatform &= OwnsTarget(bosses[i], plan.holders[i]) &&
                    (!bosses[i] || bosses[i]->GetPositionZ() <= PlatformExitZ);
            plan.rearReleased = ownedBelowPlatform;
            if (plan.rearReleased)
                LOG_INFO("playerbots", "[Naxx40] Horsemen instance={} platform clear, releasing rear support",
                    bot.GetInstanceId());
        }
        UpdateRotation(bot, plan, bosses, 0, now);
        UpdateRotation(bot, plan, bosses, 2, now);
        AllocateSupport(bot, plan, bosses);
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

    bool const holder = plan.holders[member->home] == bot.GetGUID();
    uint8 const homeCorner = AssignedCorner(member->home, plan.rotations[member->home / 2]);
    bool const standby = !holder && member->assignedCorner == NoCorner;
    uint8 const corner = holder || standby ? homeCorner : member->assignedCorner;
    Rotation const& rotation = plan.rotations[corner / 2];
    return {corner, member->assignedSlot, holder, rotation.handoff, corner >= 2 && !plan.rearReleased,
            plan.holders[AssignedCorner(corner, rotation)], standby, corner != homeCorner, member->clearingMarks};
}

class FourHorsemen40Action final : public AttackAction
{
public:
    explicit FourHorsemen40Action(PlayerbotAI* ai) : AttackAction(ai, TacticsAction), hazards(ai) {}

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

        bool const marksNeedRest = assignment.standby && assignment.clearingMarks;
        bool const reinforcing = assignment.redirected && !PlayerbotAI::IsHeal(bot);
        bool const rearSupport = reinforcing && corner >= 2;
        if (marksNeedRest)
        {
            DropAttack();
            fighting = false;
            std::array<FourHorsemenMarkSource, CornerCount> sources;
            uint8 count = 0;
            for (uint8 mark = 0; mark < CornerCount; ++mark)
                if ((assignment.clearingMarks & (1u << mark)) && bot->HasAura(Horsemen[mark].mark))
                    if (Creature const* caster = MarkCaster(bot, bosses, mark))
                        sources[count++] = {caster->GetPosition(), MarkRadius(mark)};
            return hazards.EscapeMarks(*bot, std::span(sources.data(), count));
        }

        Creature* target = bosses[corner];
        Creature const* oppositeMarker = rearSupport ? RearMarkSource(bot, bosses, corner ^ 1) : nullptr;
        bool const rearNotSettled = rearSupport && target &&
            (assignment.handoff || !AtCorner(target, corner) || !OwnsTarget(target, assignment.targetHolder) ||
             !OutsideRearMark(*target, oppositeMarker));
        bool const standby = assignment.standby || marksNeedRest;
        if (standby || rearNotSettled)
            target = nullptr;
        bool const pairAlive = target || bosses[corner ^ 1];
        bool const rearHolder = holder && corner >= 2;
        uint8 const supportCorner = assignment.handoff && !assignment.redirected ? corner ^ 1 : corner;
        Position const& cornerSpot = !holder && corner >= 2 && assignment.supportSlot < RearSupport[0].size() ?
            RearSupport[supportCorner - 2][assignment.supportSlot] : Horsemen[corner].corner;
        Position position = !holder && assignment.opening ? OpeningCamp :
            !standby && pairAlive ? cornerSpot : WaitingPosition;
        if (reinforcing && corner == 2 && assignment.supportSlot != NoSupportSlot && !standby)
            position = SpacedBlaumeuxPosition(position, 4.0f);
        if (!holder && target && corner == 2 && assignment.supportSlot == NoSupportSlot &&
            !assignment.handoff && AtCorner(target, corner))
            position = SpacedBlaumeuxPosition(target->GetPosition(), 3.0f);
        if (rearNotSettled && !standby)
        {
            // A swap or brief loss of aggro pauses attacks. The reinforcements
            // continue towards the assigned side rather than retracing the room.
            if (assignment.supportSlot < RearSupport[0].size())
                position = corner == 2 ? SpacedBlaumeuxPosition(cornerSpot, 4.0f) : cornerSpot;
            else if (bot->GetExactDist(Horsemen[corner].corner) <= RearSupportLocalRange &&
                OutsideRearMark(*bot, oppositeMarker))
                position = bot->GetPosition();
            else
                position = SpacedBlaumeuxPosition(RearSupport[0][2], 5.0f);
        }
        LogRearSupport(rearSupport, marksNeedRest, rearNotSettled, position);
        if (rearHolder && pairAlive)
        {
            position = RearDestination(corner, RearMarkSource(bot, bosses, corner ^ 1));
            if (!assignment.handoff && (!lastRearDestination ||
                lastRearDestination->GetExactDist(position) > CornerArrival))
            {
                lastRearDestination = position;
                LOG_INFO("playerbots", "[Naxx40] Horsemen rear instance={} {} routes to {} pad ({:.2f}, {:.2f}, {:.2f})",
                    bot->GetInstanceId(), bot->GetName(), Horsemen[corner].name, position.GetPositionX(),
                    position.GetPositionY(), position.GetPositionZ());
            }
            if (assignment.handoff && AI_VALUE(Unit*, "current target") != target)
                DropAttack();
        }
        fighting = holder || (target && !assignment.handoff && AtCorner(target, corner) && OwnsTarget(target, assignment.targetHolder));

        if (hazards.EscapeVoidZones(*bot, position, VoidEscapeCornerRadius))
            return true;

        uint32 const holderBuff = holder && target ? RequiredHolderBuff(*bot) : 0;
        if (holderBuff && holderBuff != RighteousFury && holderBuff != FrostPresence && botAI->CastSpell(holderBuff, bot))
        {
            LOG_INFO("playerbots", "[Naxx40] Horsemen instance={} {} prepares for {} with spell {}",
                bot->GetInstanceId(), bot->GetName(), Horsemen[corner].name, holderBuff);
            return true;
        }

        if (holder && target && target->GetVictim() != bot)
        {
            if (RaidClear::Tanks::Taunt(botAI, target))
                return true;

            if (bot->GetExactDist(target) > TauntRange || !bot->IsWithinLOSInMap(target))
            {
                if (assignment.handoff && bot->GetExactDist(Handoffs[corner / 2]) > CornerArrival)
                {
                    if (!rearHolder)
                        return WalkTo(Handoffs[corner / 2]);
                    if (WalkTo(Handoffs[corner / 2], true))
                        return true;
                }
                if (!hazards.IsSafeFromVoidZones(*bot, target->GetPosition()))
                    return WalkTo(position, rearHolder);
                return MoveNear(target, bot->IsWithinLOSInMap(target) ? TauntRange - 3.0f : 20.0f,
                    MovementPriority::MOVEMENT_COMBAT);
            }

            return AI_VALUE(Unit*, "current target") != target && Attack(target);
        }

        if (!holder && assignment.opening)
        {
            DropAttack();
            return WalkTo(position);
        }

        if (holderBuff && (holderBuff == RighteousFury || holderBuff == FrostPresence) && botAI->CastSpell(holderBuff, bot))
            return true;

        if (assignment.handoff && holder)
            return WalkTo(Handoffs[corner / 2], rearHolder);

        if (!fighting || !target)
        {
            DropAttack();
            return WalkTo(position, rearHolder);
        }

        if (WalkTo(position, rearHolder))
            return true;
        if (PlayerbotAI::IsHeal(bot))
            return false;

        if (target->GetExactDist(bot) <= AttackRange && AI_VALUE(Unit*, "current target") != target)
            return Attack(target);
        if (holder && bot->getClass() == CLASS_DRUID && bot->IsWithinMeleeRange(target))
            return botAI->CastSpell("mangle (bear)", target) || botAI->CastSpell("maul", target);
        return false;
    }

    bool AllowsOffense(Unit const* target) const
    {
        return corner != NoCorner && fighting && target && target->GetEntry() == Horsemen[corner].entry;
    }

    bool HoldingCorner() const { return holder; }
    bool HoldingRearCorner() const { return holder && corner >= 2 && corner < CornerCount; }

private:
    Position SpacedBlaumeuxPosition(Position const& center, float radius)
    {
        float const angle = float(bot->GetGUID().GetCounter() % 12) * float(M_PI) / 6.0f;
        float const x = center.GetPositionX() + std::cos(angle) * radius;
        float const y = center.GetPositionY() + std::sin(angle) * radius;
        float const z = bot->GetMapHeight(x, y, center.GetPositionZ() + 2.0f, true, 4.0f);
        if (std::abs(z - center.GetPositionZ()) <= 1.5f)
        {
            Position const position(x, y, z);
            if (hazards.IsSafeFromVoidZones(*bot, position))
                return position;
        }
        return center;
    }

    void LogRearSupport(bool rearSupport, bool marksNeedRest, bool rearNotSettled, Position const& position)
    {
        uint8 const mode = !rearSupport ? 0 : marksNeedRest ? 3 : rearNotSettled ? 2 : 1;
        if (mode == rearSupportMode)
            return;
        rearSupportMode = mode;
        if (mode)
            LOG_INFO("playerbots", "[Naxx40] Horsemen rear support instance={} {} -> {}: {} at ({:.1f}, {:.1f}, {:.2f}) marks={}",
                bot->GetInstanceId(), bot->GetName(), Horsemen[corner].name,
                mode == 3 ? "let marks expire" : mode == 2 ? "hold assigned side during handoff" : "resume assigned target",
                position.GetPositionX(), position.GetPositionY(), position.GetPositionZ(),
                bot->GetAuraCount(Horsemen[corner].mark));
    }

    Position RearDestination(uint8 corner, Creature const* otherMark)
    {
        Position const& nominal = Horsemen[corner].corner;
        Position const* best = nullptr;
        float bestDistance = -1.0f;
        auto tryPad = [&](Position const& pad)
        {
            if (!hazards.IsSafeFromVoidZones(*bot, pad))
                return false;
            float const distance = MarkDistance(pad, otherMark);
            if (!best || distance > bestDistance)
            {
                best = &pad;
                bestDistance = distance;
            }
            return OutsideRearMark(pad, otherMark);
        };

        if (tryPad(nominal))
            return nominal;
        for (Position const& pad : RearHolderPads[corner - 2])
            if (tryPad(pad))
                return pad;

        // Advance even if the opposite boss has not cleared the crossing yet.
        // The next snapshot measures both holders against the actual mark casters.
        return best ? *best : nominal;
    }

    bool WalkTo(Position const& position, bool rearHolder = false)
    {
        if (bot->GetExactDist(position) <= CornerArrival || !hazards.IsSafeFromVoidZones(*bot, position))
            return false;
        if (rearHolder && bot->isMoving() && !bot->movespline->Finalized())
        {
            G3D::Vector3 const destination = bot->movespline->FinalDestination();
            if (position.GetExactDist(destination.x, destination.y, destination.z) <= CornerArrival)
                return true;
        }
        return MoveTo(533, position.GetPositionX(), position.GetPositionY(), position.GetPositionZ(),
            false, false, rearHolder, false, MovementPriority::MOVEMENT_COMBAT);
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
    uint8 rearSupportMode = 0;
    std::optional<Position> lastRearDestination;
    FourHorsemen40Hazards hazards;
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

        auto* tactics = dynamic_cast<FourHorsemen40Action*>(context->GetAction(TacticsAction));
        if (tactics && tactics->HoldingRearCorner() && bot->getClass() == CLASS_DRUID &&
            (dynamic_cast<CastCasterFormAction*>(action) || dynamic_cast<CastCancelBearFormAction*>(action) ||
                dynamic_cast<CastCancelDireBearFormAction*>(action)))
            return 0.0f;

        Unit* target = action->GetTarget();
        auto* cast = dynamic_cast<CastSpellAction*>(action);
        SpellInfo const* spell = cast ?
            sSpellMgr->GetSpellInfo(AI_VALUE2(uint32, "spell id", cast->getSpell())) : nullptr;
        if (tactics && tactics->HoldingCorner() && spell)
        {
            if (spell->Id == BattleStance || spell->Id == BerserkerStance ||
                spell->Id == BloodPresence || spell->Id == UnholyPresence)
                return 0.0f;
            if (bot->getClass() == CLASS_DRUID && spell->HasAura(SPELL_AURA_MOD_SHAPESHIFT) &&
                spell->Id != BearForm && spell->Id != DireBearForm)
                return 0.0f;
        }

        if (!melee && !(target && bot->IsValidAttackTarget(target)) && !(spell && !spell->IsPositive()))
            return 1.0f;

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
