/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Naxx40Horsemen.h"

#include <algorithm>
#include <limits>
#include <list>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "ChooseTargetActions.h"
#include "Creature.h"
#include "EncounterHelpers.h"
#include "EncounterTankActions.h"
#include "FollowActions.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Map.h"
#include "Naxx40HorsemenGeometry.h"
#include "PetsAction.h"
#include "PlayerScript.h"
#include "Playerbots.h"
#include "ReachTargetActions.h"
#include "Spell.h"
#include "Timer.h"

namespace Naxx40Horsemen
{
namespace
{
constexpr uint32 NAXXRAMAS_MAP = 533;
constexpr auto NAXX40_DIFFICULTY = static_cast<Difficulty>(2);
constexpr uint32 DATA_HORSEMEN_ENCOUNTER = 12;
constexpr uint32 REFRESH_INTERVAL = 400;
constexpr uint32 STALE_PLAN_INTERVAL = 5 * IN_MILLISECONDS;
constexpr uint32 OPENING_SOAK_TIMEOUT = 18 * IN_MILLISECONDS;
constexpr uint8 TANK_ROTATION_STACKS = 2;
constexpr uint8 RAID_ROTATION_STACKS = 3;
constexpr uint8 DPS_ROTATION_GROUPS = 4;
constexpr float CORNER_TOLERANCE = 7.0f;
constexpr float APPROACH_DISTANCE = 5.0f;
constexpr float HANDOFF_DISTANCE = 12.0f;
constexpr float HEALER_HANDOFF_RANGE = 38.0f;
constexpr float METEOR_SOAK_DISTANCE = 5.5f;
constexpr float OPENING_SOAK_TOLERANCE = 2.5f;
constexpr float HOLY_WRATH_CLEARANCE = 15.0f;
constexpr float HOLY_WRATH_ARRIVAL_TOLERANCE = 0.5f;
constexpr float HAZARD_SEARCH_RANGE = 145.0f;
constexpr uint32 NPC_VOID_ZONE = 16697;
constexpr uint32 SPELL_METEOR = 28884;
Position const OPENING_SOAK_POSITION(2532.0f, -2993.0f, 241.42f);
constexpr std::array<Corner, CORNER_COUNT> CORNERS = {Corner::Korthazz, Corner::Mograine, Corner::Blaumeux,
                                                      Corner::Zeliek};

// Instance GUID keys, living entries, spirit entries and mark auras from Individual Progression.
struct Horseman
{
    uint32 data;
    uint32 entry;
    uint32 spirit;
    uint32 mark;
};

constexpr std::array<Horseman, CORNER_COUNT> HORSEMEN = {{
    {109, 351039, 16778, 28832},
    {106, 351037, 16775, 28834},
    {108, 351040, 16776, 28833},
    {107, 351038, 16777, 28835},
}};

constexpr std::size_t Index(Corner corner) { return static_cast<std::size_t>(corner); }
constexpr uint8 MarkBit(Corner corner) { return static_cast<uint8>(1u << Index(corner)); }

constexpr std::array<char const*, 11> NAMES = {
    "naxx40 horsemen assign corners", "naxx40 horsemen prepare tank",     "naxx40 horsemen restore tank",
    "naxx40 horsemen taunt",          "naxx40 horsemen hold corner",      "naxx40 horsemen reset marks",
    "naxx40 horsemen soak meteor",    "naxx40 horsemen escape void zone", "naxx40 horsemen spread for wrath",
    "naxx40 horsemen attack",         "naxx40 horsemen control pets",
};

struct BotPlan
{
    Corner corner = Corner::None;
    Tactic movement = Tactic::Marks;
    Position destination;
    ObjectGuid attack;
    ObjectGuid taunt;
    ObjectGuid approach;
    uint8 clearMarks = 0;
    uint8 slot = 0;
    bool tank = false;
    bool reserved = false;
    bool handoff = false;
    bool inVoidZone = false;
    bool retainCoverage = false;
};

using BotPlans = std::unordered_map<ObjectGuid, BotPlan>;

struct RaidPlan : DataMap::Base
{
    uint32 updated = 0;
    uint32 started = 0;
    bool firstMeteorSeen = false;
    bool openingSoak = true;
    uint32 meteorCast = 0;
    std::optional<Position> meteorDestination;
    ObjectGuid group;
    ObjectGuid mainTank;
    BotPlans bots;
    MarkSources markSources;
    std::vector<Position> voidZones;
    std::array<ObjectGuid, CORNER_COUNT> holders;
    std::array<ObjectGuid, CORNER_COUNT> reliefTanks;
    std::array<ObjectGuid, CORNER_COUNT> outgoingTanks;
    std::array<ObjectGuid, CORNER_COUNT> healerHolders;
};

using LiveHorsemen = std::array<Creature*, CORNER_COUNT>;
using CornerPlayers = std::array<Player*, CORNER_COUNT>;

std::string const PLAN_KEY = "playerbots.naxx40.horsemen";

bool IsActive(Player* player)
{
    return player->IsInWorld() && player->IsAlive() && IsNaxx40(player) && player->GetGroup() &&
           EncounterHelpers::IsBossInProgress(player, NAXXRAMAS_MAP, DATA_HORSEMEN_ENCOUNTER) &&
           EncounterHelpers::IsEncounterInProgress(player, NAXXRAMAS_MAP);
}

RaidPlan* GetPlan(Player* player)
{
    RaidPlan* plan = player->GetMap()->CustomData.Get<RaidPlan>(PLAN_KEY);
    return plan && plan->group == player->GetGroup()->GetGUID() ? plan : nullptr;
}

BotPlan const* FindPlan(RaidPlan const* raidPlan, Player* player)
{
    if (!raidPlan)
        return nullptr;
    auto const entry = raidPlan->bots.find(player->GetGUID());
    return entry == raidPlan->bots.end() ? nullptr : &entry->second;
}

uint32 Stacks(Player const* player, Corner corner) { return player->GetAuraCount(HORSEMEN[Index(corner)].mark); }

uint8 MarksToClear(Player const* player, Corner assignment, bool rotateEarly)
{
    uint8 mask = 0;
    for (Corner corner : CORNERS)
    {
        uint32 const stacks = Stacks(player, corner);
        if (stacks >= TANK_ROTATION_STACKS && (corner != assignment || rotateEarly || stacks >= RAID_ROTATION_STACKS))
            mask |= MarkBit(corner);
    }
    return mask;
}

bool CanEnterCorner(Player const* player, Corner corner, uint8 ownLimit, MarkSources const& sources,
                    Position const& destination)
{
    if (Stacks(player, corner) >= ownLimit)
        return false;

    constexpr float approachMargin = 4.0f;
    for (Corner other : CORNERS)
    {
        MarkSource const& source = sources[Index(other)];
        if (other != corner && source.radius && Stacks(player, other) >= TANK_ROTATION_STACKS &&
            destination.GetExactDist2d(source.position) < source.radius + approachMargin)
            return false;
    }
    return true;
}

LiveHorsemen ReadHorsemen(Player* player, RaidPlan& plan)
{
    LiveHorsemen bosses{};
    plan.voidZones.clear();
    plan.markSources = {};
    InstanceScript* instance = player->GetInstanceScript();
    if (!instance)
        return bosses;

    Creature* observer = nullptr;
    for (Corner corner : CORNERS)
    {
        Horseman const& horseman = HORSEMEN[Index(corner)];
        Creature* boss = instance->GetCreature(horseman.data);
        if (boss && boss->GetEntry() == horseman.entry)
        {
            // Use the corpse until its spirit becomes visible in the grid.
            plan.markSources[Index(corner)] = {boss->GetPosition(), MarkRadius(corner)};
            if (boss->IsAlive())
            {
                bosses[Index(corner)] = boss;
                if (!observer)
                    observer = boss;
            }
        }
    }
    if (!observer)
        return bosses;

    std::list<Creature*> hazards;
    std::vector<uint32> entries = {NPC_VOID_ZONE};
    for (Horseman const& horseman : HORSEMEN)
        entries.push_back(horseman.spirit);
    observer->GetCreatureListWithEntryInGrid(hazards, entries, HAZARD_SEARCH_RANGE);

    for (Creature* hazard : hazards)
    {
        if (!hazard->IsAlive())
            continue;
        if (hazard->GetEntry() == NPC_VOID_ZONE)
            plan.voidZones.push_back(hazard->GetPosition());
        for (Corner corner : CORNERS)
            if (!bosses[Index(corner)] && hazard->GetEntry() == HORSEMEN[Index(corner)].spirit)
                plan.markSources[Index(corner)] = {hazard->GetPosition(), MarkRadius(corner)};
    }

    return bosses;
}

void ReadMeteor(Creature* korthazz, RaidPlan& plan)
{
    std::optional<Position> destination;
    Spell* meteor = korthazz ? korthazz->FindCurrentSpellBySpellId(SPELL_METEOR) : nullptr;
    if (meteor)
    {
        plan.firstMeteorSeen = true;
        if (meteor->m_targets.HasDst())
            destination = *meteor->m_targets.GetDstPos();
    }
    if (destination && !plan.meteorDestination)
        plan.meteorCast = plan.updated;
    plan.meteorDestination = destination;
    plan.openingSoak =
        korthazz && (meteor || !plan.firstMeteorSeen) && GetMSTimeDiffToNow(plan.started) < OPENING_SOAK_TIMEOUT;
}

void SetMeteorPosition(BotPlan& assignment, Creature* boss, Player* holder, RaidPlan const& plan)
{
    assignment.movement = Tactic::Meteor;
    if (plan.meteorDestination)
        assignment.destination = *plan.meteorDestination;
    else if (plan.openingSoak)
        assignment.destination = OPENING_SOAK_POSITION;
    else if (holder && boss->GetVictim() == holder)
    {
        MarkSource const& zeliek = plan.markSources[Index(Corner::Zeliek)];
        if (!zeliek.radius || holder->GetExactDist2d(zeliek.position) > 2.0f * HOLY_WRATH_CLEARANCE)
            assignment.destination = holder->GetPosition();
    }
}

void PlanTanks(LiveHorsemen const& bosses, std::vector<Player*> const& tanks, RaidPlan& plan, CornerPlayers& holders)
{
    // DungeonClear pulls Mograine with the main tank.
    constexpr std::array<Corner, CORNER_COUNT> openingOrder = {Corner::Mograine, Corner::Korthazz, Corner::Blaumeux,
                                                               Corner::Zeliek};
    CornerPlayers primary{};
    CornerPlayers relief{};
    std::vector<Player*> spare;
    for (Player* tank : tanks)
        if (tank->IsAlive())
            spare.push_back(tank);

    auto takeTank = [&](ObjectGuid guid) -> Player*
    {
        auto entry = std::find_if(spare.begin(), spare.end(), [&](Player* tank) { return tank->GetGUID() == guid; });
        if (entry == spare.end())
            return nullptr;
        Player* tank = *entry;
        spare.erase(entry);
        return tank;
    };

    // Keep the other corners intact when a tank dies or leaves the map.
    for (Corner corner : openingOrder)
    {
        std::size_t const index = Index(corner);
        if (!bosses[index])
            continue;
        primary[index] = takeTank(plan.holders[index]);
        relief[index] = takeTank(plan.reliefTanks[index]);
        if (relief[index] && relief[index] == bosses[index]->GetVictim())
        {
            if (primary[index])
                plan.outgoingTanks[index] = primary[index]->GetGUID();
            std::swap(primary[index], relief[index]);
        }
        else if (!primary[index])
            std::swap(primary[index], relief[index]);
    }

    auto assignSpare = [&](CornerPlayers& seats, bool frontTanksOnly)
    {
        for (Corner corner : openingOrder)
        {
            std::size_t const index = Index(corner);
            if (!bosses[index] || seats[index] || spare.empty())
                continue;
            if (frontTanksOnly && corner != Corner::Korthazz && corner != Corner::Mograine)
                continue;
            auto score = [&](Player* tank)
            {
                return std::tuple{!PlayerbotAI::IsTank(tank),
                                  corner == Corner::Mograine && tank->GetGUID() != plan.mainTank, Stacks(tank, corner),
                                  std::numeric_limits<uint32>::max() - tank->GetMaxHealth()};
            };
            auto best = std::min_element(spare.begin(), spare.end(),
                                         [&](Player* left, Player* right) { return score(left) < score(right); });
            if (frontTanksOnly && !PlayerbotAI::IsTank(*best))
                continue;
            seats[index] = *best;
            spare.erase(best);
        }
    };

    assignSpare(primary, true);
    assignSpare(relief, true);
    assignSpare(primary, false);
    for (Corner corner : openingOrder)
    {
        std::size_t const index = Index(corner);
        if (!bosses[index] || primary[index])
            continue;

        // A covered corner can lend its relief tank when both tanks at another corner are gone.
        auto marks = [&](Player* tank) { return tank ? Stacks(tank, corner) : std::numeric_limits<uint32>::max(); };
        auto donor = std::min_element(relief.begin(), relief.end(),
                                      [&](Player* left, Player* right) { return marks(left) < marks(right); });
        primary[index] = std::exchange(*donor, nullptr);
    }
    assignSpare(relief, false);

    for (Corner corner : openingOrder)
    {
        std::size_t const index = Index(corner);
        Creature* boss = bosses[index];
        if (!boss || !primary[index])
        {
            plan.holders[index].Clear();
            plan.reliefTanks[index].Clear();
            plan.outgoingTanks[index].Clear();
            continue;
        }
        Player* active = primary[index];
        Player* next = relief[index];
        Unit* victim = boss->GetVictim();
        if (next && (next == victim || (active != victim && next->GetGUID() == plan.holders[index])))
            std::swap(active, next);

        ObjectGuid& outgoingGuid = plan.outgoingTanks[index];
        if (outgoingGuid)
        {
            auto const outgoing =
                std::find_if(tanks.begin(), tanks.end(), [&](Player* tank) { return tank->GetGUID() == outgoingGuid; });
            if (outgoing == tanks.end() || !(*outgoing)->IsAlive() || !Stacks(*outgoing, corner))
                outgoingGuid.Clear();
            else
            {
                BotPlan& waiting = plan.bots.at(outgoingGuid);
                waiting.reserved = true;
                waiting.clearMarks |= MarkBit(corner);
            }
        }

        uint32 const holderStacks = Stacks(active, corner);
        bool const needsRelief = holderStacks >= TANK_ROTATION_STACKS;
        auto canRelieve = [&](Player* tank)
        {
            uint32 const stacks = Stacks(tank, corner);
            return stacks < TANK_ROTATION_STACKS && tank->GetGUID() != outgoingGuid;
        };
        if (needsRelief && (!next || !canRelieve(next)))
        {
            auto fresh = std::find_if(spare.begin(), spare.end(), canRelieve);
            if (fresh != spare.end())
            {
                Player* replacement = *fresh;
                if (next)
                    *fresh = next;
                else
                    spare.erase(fresh);
                next = replacement;
            }
        }

        holders[index] = active;
        plan.holders[index] = active->GetGUID();
        plan.reliefTanks[index] = next ? next->GetGUID() : ObjectGuid::Empty;
        BotPlan& tankPlan = plan.bots.at(active->GetGUID());
        tankPlan.tank = true;
        tankPlan.reserved = true;
        tankPlan.corner = corner;
        tankPlan.attack = boss->GetGUID();
        tankPlan.taunt = boss->GetGUID();
        tankPlan.movement = Tactic::Corner;
        tankPlan.destination = CornerPosition(corner);
        if (victim != active)
            tankPlan.approach = boss->GetGUID();
        else if (corner == Corner::Korthazz && (plan.openingSoak || plan.meteorDestination))
            SetMeteorPosition(tankPlan, boss, active, plan);

        if (next)
        {
            BotPlan& nextPlan = plan.bots.at(next->GetGUID());
            nextPlan.reserved = true;
            // The original holder keeps attacking until the relief tank has threat.
            uint32 const reliefStacks = Stacks(next, corner);
            if (holderStacks && canRelieve(next))
            {
                nextPlan.tank = true;
                nextPlan.handoff = true;
                nextPlan.corner = corner;
                nextPlan.movement = Tactic::Corner;
                nextPlan.approach = boss->GetGUID();
                // If both have the first mark, take over before the next pulse disqualifies the relief.
                if (needsRelief || reliefStacks)
                {
                    if (victim == active)
                        tankPlan.taunt.Clear();
                    nextPlan.taunt = boss->GetGUID();
                }
            }
        }
    }
}

void PlanSupport(Player* member, Corner corner, Creature* boss, Player* holder, uint8 slot, bool healer, RaidPlan& plan)
{
    BotPlan& assignment = plan.bots.at(member->GetGUID());
    assignment.corner = corner;
    assignment.destination = SupportPosition(corner, slot);
    assignment.movement = Tactic::Corner;

    if (corner == Corner::Korthazz)
        SetMeteorPosition(assignment, boss, holder, plan);
    else if (corner == Corner::Zeliek)
        assignment.movement = Tactic::HolyWrath;
    else if (!healer && PlayerbotAI::IsMelee(member))
        assignment.approach = boss->GetGUID();

    Position const& holdPosition =
        corner == Corner::Korthazz && plan.openingSoak ? OPENING_SOAK_POSITION : CornerPosition(corner);
    if (!healer && holder && holder == boss->GetVictim() &&
        boss->GetExactDist2d(holdPosition) <= CORNER_TOLERANCE + APPROACH_DISTANCE)
        assignment.attack = boss->GetGUID();
}

using HealerChoices = std::array<std::vector<Player*>, CORNER_COUNT>;
using SupportSlots = std::array<uint8, CORNER_COUNT>;

bool CoverCorner(std::size_t corner, HealerChoices const& choices, CornerPlayers& healers,
                 std::array<bool, CORNER_COUNT>& visited)
{
    if (visited[corner])
        return false;
    visited[corner] = true;

    for (Player* healer : choices[corner])
    {
        if (std::find(healers.begin(), healers.end(), healer) == healers.end())
        {
            healers[corner] = healer;
            return true;
        }
    }

    // Free an eligible healer by moving another corner to one of its alternatives.
    for (Player* healer : choices[corner])
    {
        auto current = std::find(healers.begin(), healers.end(), healer);
        if (current != healers.end() &&
            CoverCorner(static_cast<std::size_t>(current - healers.begin()), choices, healers, visited))
        {
            healers[corner] = healer;
            return true;
        }
    }
    return false;
}

bool InHealingPosition(Player const* healer, Player const* holder)
{
    return healer && holder && healer->GetExactDist(holder) <= HEALER_HANDOFF_RANGE && healer->IsWithinLOSInMap(holder);
}

void PlanHealers(LiveHorsemen const& bosses, CornerPlayers const& holders, std::vector<Player*> const& healers,
                 BotPlans const& previous, RaidPlan& plan, SupportSlots& slots)
{
    HealerChoices choices;
    CornerPlayers coverage{};
    std::size_t liveCount = 0;
    for (Corner corner : CORNERS)
    {
        std::size_t const index = Index(corner);
        if (!bosses[index])
            continue;
        ++liveCount;

        auto& eligible = choices[index];
        for (Player* healer : healers)
            if (CanEnterCorner(healer, corner, RAID_ROTATION_STACKS, plan.markSources, SupportPosition(corner, 0)))
                eligible.push_back(healer);

        auto score = [&](Player* healer)
        {
            auto const old = previous.find(healer->GetGUID());
            uint32 const stacks = Stacks(healer, corner);
            return 2 * stacks + (stacks >= TANK_ROTATION_STACKS ? 10 : 0) +
                   (old == previous.end() || old->second.corner != corner ? 5 : 0);
        };
        std::stable_sort(eligible.begin(), eligible.end(),
                         [&](Player* left, Player* right) { return score(left) < score(right); });
    }

    CornerPlayers incumbents{};
    for (std::size_t index = 0; index < incumbents.size(); ++index)
    {
        if (!bosses[index])
            continue;
        auto previousHealer = std::find_if(healers.begin(), healers.end(), [&](Player* healer)
                                           { return healer->GetGUID() == plan.healerHolders[index]; });
        if (previousHealer != healers.end() && InHealingPosition(*previousHealer, holders[index]))
            incumbents[index] = *previousHealer;
    }

    CornerPlayers guards{};
    bool addedGuard;
    // Recheck every corner after a rematch, which can move a previously retained healer.
    do
    {
        coverage = {};
        for (std::size_t index = 0; index < coverage.size(); ++index)
        {
            std::array<bool, CORNER_COUNT> visited{};
            CoverCorner(index, choices, coverage, visited);
        }

        addedGuard = false;
        for (std::size_t index = 0; index < coverage.size(); ++index)
        {
            Player* incumbent = incumbents[index];
            if (!incumbent || guards[index] || incumbent == coverage[index] ||
                InHealingPosition(coverage[index], holders[index]))
                continue;

            guards[index] = incumbent;
            for (auto& eligible : choices)
                std::erase(eligible, incumbent);
            addedGuard = true;
        }
    } while (addedGuard);

    std::size_t assigned = 0;
    for (std::size_t index = 0; index < coverage.size(); ++index)
    {
        Corner const corner = CORNERS[index];
        Player* incumbent = guards[index];
        Player* replacement = coverage[index];
        if (incumbent)
        {
            BotPlan& guarding = plan.bots.at(incumbent->GetGUID());
            guarding.corner = corner;
            guarding.movement = Tactic::Corner;
            guarding.destination = incumbent->GetPosition();
            guarding.retainCoverage = true;
            ++slots[index];
            ++assigned;
        }
        else
            plan.healerHolders[index] = replacement ? replacement->GetGUID() : ObjectGuid::Empty;

        if (replacement)
        {
            uint8 slot = slots[index]++;
            if (incumbent && corner == Corner::Zeliek &&
                incumbent->GetExactDist2d(SupportPosition(corner, 0)) >
                    incumbent->GetExactDist2d(SupportPosition(corner, 1)))
                slot = 0;
            PlanSupport(replacement, corner, bosses[index], holders[index], slot, true, plan);
            ++assigned;
        }
    }

    for (Corner corner : CORNERS)
    {
        if (assigned + liveCount >= healers.size())
            break;
        std::size_t const index = Index(corner);
        auto const& eligible = choices[index];
        auto spare = std::find_if(eligible.begin(), eligible.end(),
                                  [&](Player* healer)
                                  {
                                      return plan.bots.at(healer->GetGUID()).corner == Corner::None &&
                                             CanEnterCorner(healer, corner, TANK_ROTATION_STACKS, plan.markSources,
                                                            SupportPosition(corner, slots[index]));
                                  });
        if (spare != eligible.end())
        {
            PlanSupport(*spare, corner, bosses[index], holders[index], slots[index]++, true, plan);
            ++assigned;
        }
    }
}

void RefreshPlan(Player* player)
{
    RaidPlan& plan = *player->GetMap()->CustomData.GetDefault<RaidPlan>(PLAN_KEY);
    auto previous = std::move(plan.bots);
    plan.bots.clear();
    Group* group = player->GetGroup();
    bool const newAttempt = plan.group != group->GetGUID() || GetMSTimeDiffToNow(plan.updated) > STALE_PLAN_INTERVAL;
    if (newAttempt)
    {
        plan.holders = {};
        plan.reliefTanks = {};
        plan.outgoingTanks = {};
        plan.healerHolders = {};
        plan.mainTank.Clear();
        plan.started = getMSTime();
        plan.firstMeteorSeen = false;
        plan.meteorDestination.reset();
        previous.clear();
    }
    plan.group = group->GetGUID();
    plan.updated = getMSTime();
    LiveHorsemen const bosses = ReadHorsemen(player, plan);
    ReadMeteor(bosses[Index(Corner::Korthazz)], plan);

    std::vector<Player*> tanks;
    std::vector<Player*> reserves;
    std::vector<Player*> healers;
    std::vector<Player*> damage;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || member->GetMap() != player->GetMap() || !GET_PLAYERBOT_AI(member))
            continue;

        bool const tank = PlayerbotAI::IsTank(member);
        if (tank)
            tanks.push_back(member);
        bool const healer = !tank && PlayerbotAI::IsHeal(member);
        if (!tank && !healer && PlayerbotAI::IsMelee(member) &&
            EncounterTankActions::CanTaunt(GET_PLAYERBOT_AI(member)))
            reserves.push_back(member);
        if (!member->IsAlive())
            continue;

        BotPlan& assignment = plan.bots[member->GetGUID()];
        assignment.clearMarks = MarksToClear(member, Corner::None, true);
        assignment.slot = static_cast<uint8>(std::hash<ObjectGuid>{}(member->GetGUID()));
        assignment.inVoidZone = IsNearVoidZone(member->GetPosition(), plan.voidZones);
        if (!tank)
        {
            if (healer)
                healers.push_back(member);
            else
                damage.push_back(member);
        }
    }

    if (newAttempt)
    {
        auto const& slots = group->GetMemberSlots();
        auto mainTank = std::find_if(slots.begin(), slots.end(), [](Group::MemberSlot const& member)
                                     { return member.flags & MEMBER_FLAG_MAINTANK; });
        if (mainTank != slots.end())
            plan.mainTank = mainTank->guid;
        else if (auto first = std::find_if(tanks.begin(), tanks.end(), [](Player* tank) { return tank->IsAlive(); });
                 first != tanks.end())
            plan.mainTank = (*first)->GetGUID();
    }
    std::stable_partition(tanks.begin(), tanks.end(), [&](Player* tank) { return tank->GetGUID() == plan.mainTank; });
    tanks.insert(tanks.end(), reserves.begin(), reserves.end());
    CornerPlayers holders{};
    PlanTanks(bosses, tanks, plan, holders);

    SupportSlots supportSlots{};
    PlanHealers(bosses, holders, healers, previous, plan, supportSlots);

    for (Corner corner : CORNERS)
    {
        std::size_t const index = Index(corner);
        if (!bosses[index])
            continue;
        for (Player* member : damage)
        {
            uint8& supportSlot = supportSlots[index];
            BotPlan& assignment = plan.bots.at(member->GetGUID());
            if (assignment.reserved ||
                (corner == Corner::Zeliek && (supportSlot >= ZELIEK_SUPPORT_SLOTS || PlayerbotAI::IsMelee(member))))
                continue;

            BotPlan const waiting = assignment;
            PlanSupport(member, corner, bosses[index], holders[index], supportSlot, false, plan);
            uint8 const cohort = assignment.slot % DPS_ROTATION_GROUPS;
            uint8 const ownLimit = std::min<uint8>(cohort + 1, RAID_ROTATION_STACKS);
            Position const& arrival = assignment.approach ? bosses[index]->GetPosition() : assignment.destination;
            if (CanEnterCorner(member, corner, ownLimit, plan.markSources, arrival))
                ++supportSlot;
            else
            {
                assignment = waiting;
                if (Stacks(member, corner) >= ownLimit)
                    assignment.clearMarks |= MarkBit(corner);
            }
        }
        break;
    }
}

Creature* Resolve(Player* player, ObjectGuid guid)
{
    Creature* creature = guid ? player->GetMap()->GetCreature(guid) : nullptr;
    return creature && creature->IsAlive() ? creature : nullptr;
}

bool ControlsTank(Player* player)
{
    BotPlan const* plan = IsActive(player) ? FindPlan(GetPlan(player), player) : nullptr;
    return plan && plan->tank;
}

std::string const RESTORATION_KEY = "playerbots.naxx40.horsemen.restore-scheduled";

struct ScheduledRestoration : DataMap::Base
{
};

void ScheduleRestore(Player* player)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);
    if (!botAI || IsNaxx40(player) || player->CustomData.Get<ScheduledRestoration>(RESTORATION_KEY) ||
        !EncounterTankActions::HasTemporaryState(botAI))
        return;

    player->CustomData.GetDefault<ScheduledRestoration>(RESTORATION_KEY);
    player->m_Events.AddEventAtOffset(
        [player]
        {
            player->CustomData.Erase(RESTORATION_KEY);
            PlayerbotAI* currentAI = GET_PLAYERBOT_AI(player);
            if (!currentAI || !player->IsInWorld() || !player->IsAlive() || IsNaxx40(player))
                return;

            EncounterTankActions::Restore(currentAI);
            ScheduleRestore(player);
        },
        Seconds(1));
}

class HorsemenCleanupScript : public PlayerScript
{
public:
    HorsemenCleanupScript()
        : PlayerScript("HorsemenCleanupScript", {PLAYERHOOK_ON_MAP_CHANGED, PLAYERHOOK_ON_PLAYER_RESURRECT})
    {
    }

    void OnPlayerMapChanged(Player* player) override { ScheduleRestore(player); }
    void OnPlayerResurrect(Player* player, float /*restorePercent*/, bool& /*applySickness*/) override
    {
        ScheduleRestore(player);
    }
};

}  // namespace

void AddScripts() { new HorsemenCleanupScript(); }

char const* Name(Tactic tactic) { return NAMES[static_cast<std::size_t>(tactic)]; }

bool IsNaxx40(Player* player)
{
    return player->IsInWorld() && player->GetMapId() == NAXXRAMAS_MAP &&
           player->GetMap()->GetDifficulty() == NAXX40_DIFFICULTY;
}

void InitTriggers(std::vector<TriggerNode*>& triggers)
{
    auto add = [&](Tactic tactic, float priority)
    { triggers.push_back(new TriggerNode(Name(tactic), {NextAction(Name(tactic), priority)})); };
    add(Tactic::Assign, ACTION_EMERGENCY + 9);
    add(Tactic::VoidZone, ACTION_EMERGENCY + 8);
    add(Tactic::Meteor, ACTION_EMERGENCY + 2);
    add(Tactic::Prepare, ACTION_RAID + 8);
    add(Tactic::Taunt, ACTION_RAID + 7);
    add(Tactic::Marks, ACTION_RAID + 6);
    add(Tactic::HolyWrath, ACTION_RAID + 5);
    add(Tactic::Restore, ACTION_RAID + 4);
    add(Tactic::Corner, ACTION_RAID + 3);
    add(Tactic::Pets, ACTION_RAID + 3);
    add(Tactic::Attack, ACTION_RAID + 2);
}

template <Tactic tactic>
bool HorsemenTrigger<tactic>::IsActive()
{
    if constexpr (tactic == Tactic::Restore)
        return IsNaxx40(bot) && EncounterTankActions::HasTemporaryState(botAI) && !ControlsTank(bot);
    if constexpr (tactic == Tactic::Pets)
        return !bot->m_Controlled.empty() && Naxx40Horsemen::IsActive(bot);

    if (!Naxx40Horsemen::IsActive(bot))
        return false;

    RaidPlan const* plan = GetPlan(bot);
    if constexpr (tactic == Tactic::Assign)
        return !plan || GetMSTimeDiffToNow(plan->updated) >= REFRESH_INTERVAL;

    BotPlan const* assignment = FindPlan(plan, bot);
    if (!assignment)
        return false;

    if constexpr (tactic == Tactic::VoidZone)
        return assignment->inVoidZone;
    if constexpr (tactic == Tactic::Prepare)
        return assignment->tank && EncounterTankActions::NeedsPreparation(botAI);
    if constexpr (tactic == Tactic::Taunt)
        return !assignment->taunt.IsEmpty();
    if constexpr (tactic == Tactic::Attack)
        return !assignment->attack.IsEmpty();
    return assignment->movement == tactic;
}

bool AssignCornersAction::Execute(Event /*event*/)
{
    if (IsActive(bot))
    {
        RaidPlan const* plan = GetPlan(bot);
        if (!plan || GetMSTimeDiffToNow(plan->updated) >= REFRESH_INTERVAL)
            RefreshPlan(bot);
    }
    return false;
}

bool TauntHorsemanAction::Execute(Event /*event*/)
{
    if (!IsActive(bot))
        return false;

    BotPlan const* plan = FindPlan(GetPlan(bot), bot);
    Creature* target = plan ? Resolve(bot, plan->taunt) : nullptr;
    if (!target || (plan->handoff && !bot->IsWithinDistInMap(target, HANDOFF_DISTANCE)))
        return false;
    return EncounterTankActions::TryTaunt(botAI, target);
}

bool PrepareHorsemanTankAction::Execute(Event /*event*/)
{
    return ControlsTank(bot) && EncounterTankActions::Prepare(botAI);
}

bool RestoreHorsemanTankAction::Execute(Event /*event*/)
{
    return !ControlsTank(bot) && EncounterTankActions::Restore(botAI);
}

bool ControlHorsemanPetsAction::Execute(Event /*event*/)
{
    if (!IsActive(bot))
        return false;

    BotPlan const* plan = FindPlan(GetPlan(bot), bot);
    if (!plan)
        return false;

    Creature* target = Resolve(bot, plan->attack);
    bool commanded = false;
    bool needsRecall = false;
    for (Unit* unit : bot->m_Controlled)
    {
        Creature* minion = unit->ToCreature();
        if (!minion || !minion->GetCharmInfo() || !minion->IsAlive())
            continue;
        if (target)
            commanded |= EncounterHelpers::CommandPetAttack(minion, target);
        else if (minion->GetVictim())
            needsRecall = true;
    }
    if (needsRecall)
        EncounterHelpers::CallBackPets(bot);
    return commanded || needsRecall;
}

template <Tactic tactic>
bool HorsemenMoveAction<tactic>::isUseful()
{
    if (!IsActive(bot) || !IsMovingAllowed())
        return false;

    BotPlan const* plan = FindPlan(GetPlan(bot), bot);
    if (!plan)
        return false;

    if constexpr (tactic == Tactic::VoidZone)
        return plan->inVoidZone;
    return plan->movement == tactic;
}

template <Tactic tactic>
bool HorsemenMoveAction<tactic>::Execute(Event /*event*/)
{
    if (!isUseful())
        return false;

    RaidPlan const* raid = GetPlan(bot);
    BotPlan const* plan = FindPlan(raid, bot);
    if (!plan)
        return false;

    Position destination;
    float tolerance = 2.5f;
    bool const holdingMeteor = tactic == Tactic::Meteor && (raid->openingSoak || raid->meteorDestination);
    MovementPriority const priority = (holdingMeteor || tactic == Tactic::VoidZone || tactic == Tactic::Marks)
                                          ? MovementPriority::MOVEMENT_FORCED
                                          : MovementPriority::MOVEMENT_COMBAT;
    if constexpr (tactic == Tactic::VoidZone)
    {
        if (!IsNearVoidZone(bot->GetPosition(), raid->voidZones))
            return false;

        std::optional<Position> escape = VoidZoneEscape(bot, raid->voidZones);
        if (!escape)
            return false;
        destination = *escape;
    }
    else
    {
        destination = plan->destination;
        if constexpr (tactic == Tactic::Meteor)
        {
            tolerance = raid->openingSoak && !raid->meteorDestination ? OPENING_SOAK_TOLERANCE : METEOR_SOAK_DISTANCE;
            if (raid->meteorDestination && raid->meteorCast != _meteorCast)
            {
                _meteorCast = raid->meteorCast;
                context->GetValue<LastMovement&>("last movement")->Get().clear();
            }
            if (holdingMeteor && bot->GetExactDist(destination) <= tolerance && bot->isMoving())
            {
                bot->StopMoving();
                context->GetValue<LastMovement&>("last movement")->Get().clear();
                return true;
            }
        }
        if constexpr (tactic == Tactic::Marks)
            EncounterHelpers::StopAttackingAndChanneling(bot);
        if (IsWaitingForLastMove(priority))
            return false;

        if constexpr (tactic == Tactic::Marks)
        {
            destination = WaitPosition(bot, raid->markSources, raid->voidZones, plan->clearMarks, plan->slot);
            tolerance = HOLY_WRATH_ARRIVAL_TOLERANCE;
        }
        else if constexpr (tactic == Tactic::HolyWrath)
            tolerance = HOLY_WRATH_ARRIVAL_TOLERANCE;
        else if constexpr (tactic == Tactic::Corner)
        {
            if (plan->corner == Corner::Zeliek)
                tolerance = HOLY_WRATH_ARRIVAL_TOLERANCE;
            if (plan->approach)
            {
                Creature const* approach = Resolve(bot, plan->approach);
                if (!approach)
                    return false;

                if (bot->GetExactDist2d(approach) <= APPROACH_DISTANCE)
                    return false;
                destination = approach->GetPosition();
                tolerance = APPROACH_DISTANCE;
            }
        }
    }

    if (bot->GetExactDist(&destination) <= tolerance)
        return false;

    if constexpr (tactic == Tactic::VoidZone)
        context->GetValue<LastMovement&>("last movement")->Get().clear();
    else if constexpr (tactic != Tactic::Marks)
    {
        std::optional<Position> waypoint = RouteAroundVoidZones(bot, destination, raid->voidZones);
        if (!waypoint)
        {
            if (bot->isMoving())
                bot->StopMoving();
            return false;
        }
        destination = *waypoint;
    }

    return MoveTo(NAXXRAMAS_MAP, destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ(),
                  false, false, false, true, priority, true);
}

bool AttackHorsemanAction::Execute(Event /*event*/)
{
    if (!IsActive(bot))
        return false;

    BotPlan const* plan = FindPlan(GetPlan(bot), bot);
    Creature* target = plan ? Resolve(bot, plan->attack) : nullptr;
    return target && Attack(target);
}

float HorsemenFormationMultiplier::GetValue(Action* action)
{
    if (!IsActive(bot))
        return 1.0f;

    RaidPlan const* raid = GetPlan(bot);
    BotPlan const* plan = FindPlan(raid, bot);
    if (!plan)
        return 1.0f;

    if (dynamic_cast<DpsAssistAction*>(action) || dynamic_cast<TankAssistAction*>(action) ||
        dynamic_cast<FollowAction*>(action) || dynamic_cast<CombatFormationMoveAction*>(action) ||
        dynamic_cast<FleeAction*>(action) || dynamic_cast<CastReachTargetSpellAction*>(action) ||
        dynamic_cast<PetAttackAction*>(action) || dynamic_cast<SetPetStanceAction*>(action))
        return 0.0f;

    if (EncounterTankActions::ConflictsWithTankControl(botAI, action, plan->tank))
        return 0.0f;

    if (auto* reach = dynamic_cast<ReachTargetAction*>(action))
    {
        if (plan->movement == Tactic::Meteor && (raid->openingSoak || raid->meteorDestination))
            return 0.0f;

        Unit* target = reach->GetTarget();
        if (target && (!bot->IsFriendlyTo(target) || plan->retainCoverage || plan->movement == Tactic::HolyWrath ||
                       (plan->movement == Tactic::Marks && plan->clearMarks)))
            return 0.0f;
    }

    if (dynamic_cast<AttackHorsemanAction*>(action) || dynamic_cast<TauntHorsemanAction*>(action) ||
        dynamic_cast<DropTargetAction*>(action))
        return 1.0f;

    if (EncounterHelpers::IsDpsHoldCandidate(bot, action))
    {
        Unit* target = action->GetTarget();
        if (target && bot->IsFriendlyTo(target))
            return 1.0f;
        if (!plan->attack || (target && target->GetGUID() != plan->attack))
            return 0.0f;
    }
    return 1.0f;
}

template class HorsemenTrigger<Tactic::Assign>;
template class HorsemenTrigger<Tactic::Prepare>;
template class HorsemenTrigger<Tactic::Restore>;
template class HorsemenTrigger<Tactic::Taunt>;
template class HorsemenTrigger<Tactic::Corner>;
template class HorsemenTrigger<Tactic::Marks>;
template class HorsemenTrigger<Tactic::Meteor>;
template class HorsemenTrigger<Tactic::VoidZone>;
template class HorsemenTrigger<Tactic::HolyWrath>;
template class HorsemenTrigger<Tactic::Attack>;
template class HorsemenTrigger<Tactic::Pets>;

template class HorsemenMoveAction<Tactic::Corner>;
template class HorsemenMoveAction<Tactic::Marks>;
template class HorsemenMoveAction<Tactic::Meteor>;
template class HorsemenMoveAction<Tactic::VoidZone>;
template class HorsemenMoveAction<Tactic::HolyWrath>;
}  // namespace Naxx40Horsemen

void AddSC_Naxx40HorsemenBotScripts() { Naxx40Horsemen::AddScripts(); }
