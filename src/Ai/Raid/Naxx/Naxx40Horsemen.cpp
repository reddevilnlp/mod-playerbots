/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Naxx40Horsemen.h"

#include <algorithm>
#include <limits>
#include <list>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "ChooseTargetActions.h"
#include "Creature.h"
#include "DruidShapeshiftActions.h"
#include "EncounterHelpers.h"
#include "FollowActions.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Map.h"
#include "Naxx40HorsemenGeometry.h"
#include "Playerbots.h"
#include "ReachTargetActions.h"
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
constexpr uint8 TANK_ROTATION_STACKS = 2;
constexpr uint8 RAID_ROTATION_STACKS = 3;
constexpr float CORNER_TOLERANCE = 7.0f;
constexpr float APPROACH_DISTANCE = 5.0f;
constexpr float METEOR_SOAK_DISTANCE = 5.5f;
constexpr float HOLY_WRATH_CLEARANCE = 15.0f;
constexpr float HOLY_WRATH_ARRIVAL_TOLERANCE = 0.5f;
constexpr float HAZARD_SEARCH_RANGE = 145.0f;
constexpr uint32 NPC_VOID_ZONE = 16697;
constexpr uint32 SPELL_TAUNT = 355;
constexpr uint32 SPELL_GROWL = 6795;
constexpr uint32 SPELL_HAND_OF_RECKONING = 62124;
constexpr uint32 SPELL_DARK_COMMAND = 56222;
constexpr uint32 SPELL_RIGHTEOUS_FURY = 25780;
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

constexpr std::array<char const*, 8> NAMES = {
    "naxx40 horsemen assign corners",   "naxx40 horsemen taunt",       "naxx40 horsemen hold corner",
    "naxx40 horsemen reset marks",      "naxx40 horsemen soak meteor", "naxx40 horsemen escape void zone",
    "naxx40 horsemen spread for wrath", "naxx40 horsemen attack",
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
};

using BotPlans = std::unordered_map<ObjectGuid, BotPlan>;

struct RaidPlan : DataMap::Base
{
    uint32 updated = 0;
    ObjectGuid group;
    ObjectGuid mainTank;
    BotPlans bots;
    MarkSources markSources;
    std::vector<Position> voidZones;
    std::array<ObjectGuid, CORNER_COUNT> holders;
    std::array<ObjectGuid, CORNER_COUNT> reliefTanks;
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
        if (boss && boss->IsAlive() && boss->GetEntry() == horseman.entry)
        {
            bosses[Index(corner)] = boss;
            plan.markSources[Index(corner)] = {boss->GetPosition(), MarkRadius(corner)};
            if (!observer)
                observer = boss;
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
        if (!primary[index] || (relief[index] && relief[index] == bosses[index]->GetVictim()))
            std::swap(primary[index], relief[index]);
    }

    auto assignSpare = [&](CornerPlayers& seats)
    {
        for (Corner corner : openingOrder)
        {
            std::size_t const index = Index(corner);
            if (!bosses[index] || seats[index] || spare.empty())
                continue;
            auto best = std::min_element(spare.begin(), spare.end(), [&](Player* left, Player* right)
                                         { return Stacks(left, corner) < Stacks(right, corner); });
            seats[index] = *best;
            spare.erase(best);
        }
    };

    assignSpare(primary);
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
    assignSpare(relief);

    for (Corner corner : openingOrder)
    {
        std::size_t const index = Index(corner);
        Creature* boss = bosses[index];
        if (!boss || !primary[index])
        {
            plan.holders[index].Clear();
            plan.reliefTanks[index].Clear();
            continue;
        }
        Player* active = primary[index];
        Player* next = relief[index];
        Unit* victim = boss->GetVictim();
        if (next && (next == victim || (active != victim && next->GetGUID() == plan.holders[index])))
            std::swap(active, next);

        bool const needsRelief = Stacks(active, corner) >= TANK_ROTATION_STACKS;
        if (needsRelief && next && Stacks(next, corner) >= TANK_ROTATION_STACKS)
        {
            auto fresh = std::find_if(spare.begin(), spare.end(),
                                      [&](Player* tank) { return Stacks(tank, corner) < TANK_ROTATION_STACKS; });
            if (fresh != spare.end())
                std::swap(next, *fresh);
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

        if (next)
        {
            BotPlan& nextPlan = plan.bots.at(next->GetGUID());
            nextPlan.reserved = true;
            // The original holder keeps attacking until the relief tank has threat.
            if (needsRelief && Stacks(next, corner) < TANK_ROTATION_STACKS)
            {
                tankPlan.taunt.Clear();
                nextPlan.tank = true;
                nextPlan.handoff = true;
                nextPlan.corner = corner;
                nextPlan.movement = Tactic::Corner;
                nextPlan.taunt = boss->GetGUID();
                nextPlan.approach = boss->GetGUID();
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
    {
        assignment.movement = Tactic::Meteor;
        Unit* victim = boss->GetVictim();
        MarkSource const& zeliek = plan.markSources[Index(Corner::Zeliek)];
        if (victim && victim == holder &&
            (!zeliek.radius || victim->GetExactDist2d(zeliek.position) > 2.0f * HOLY_WRATH_CLEARANCE))
            assignment.destination = victim->GetPosition();
    }
    else if (corner == Corner::Zeliek)
        assignment.movement = Tactic::HolyWrath;
    else if (!healer && PlayerbotAI::IsMelee(member))
        assignment.approach = boss->GetGUID();

    if (!healer && holder && holder == boss->GetVictim() &&
        boss->GetExactDist2d(CornerPosition(corner)) <= CORNER_TOLERANCE + APPROACH_DISTANCE)
        assignment.attack = boss->GetGUID();
}

bool CanRelieveTank(Player* player)
{
    switch (player->getClass())
    {
        case CLASS_WARRIOR:
            return player->HasSpell(SPELL_TAUNT);
        case CLASS_DRUID:
            return PlayerbotAI::IsMelee(player) && player->HasSpell(SPELL_GROWL);
        case CLASS_PALADIN:
            return player->HasSpell(SPELL_HAND_OF_RECKONING);
        case CLASS_DEATH_KNIGHT:
            return player->HasSpell(SPELL_DARK_COMMAND);
        default:
            return false;
    }
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
            if (!MarksToClear(healer, corner, false))
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
        std::array<bool, CORNER_COUNT> visited{};
        CoverCorner(index, choices, coverage, visited);
    }

    std::size_t assigned = 0;
    for (std::size_t index = 0; index < coverage.size(); ++index)
    {
        if (coverage[index])
        {
            PlanSupport(coverage[index], CORNERS[index], bosses[index], holders[index], slots[index]++, true, plan);
            ++assigned;
        }
    }

    for (Corner corner : CORNERS)
    {
        if (assigned + liveCount >= healers.size())
            break;
        std::size_t const index = Index(corner);
        auto const& eligible = choices[index];
        auto spare = std::find_if(
            eligible.begin(), eligible.end(), [&](Player* healer)
            { return plan.bots.at(healer->GetGUID()).corner == Corner::None && !MarksToClear(healer, corner, true); });
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
        plan.mainTank.Clear();
        previous.clear();
    }
    plan.group = group->GetGUID();
    plan.updated = getMSTime();
    LiveHorsemen const bosses = ReadHorsemen(player, plan);

    std::vector<Player*> tanks;
    std::vector<Player*> reserves;
    std::vector<Player*> healers;
    std::vector<Player*> damage;
    uint8 slot = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || member->GetMap() != player->GetMap() || !GET_PLAYERBOT_AI(member))
            continue;

        bool const tank = PlayerbotAI::IsTank(member);
        if (tank)
            tanks.push_back(member);
        bool const healer = !tank && PlayerbotAI::IsHeal(member);
        if (!tank && !healer && CanRelieveTank(member))
            reserves.push_back(member);
        if (!member->IsAlive())
            continue;

        BotPlan& assignment = plan.bots[member->GetGUID()];
        assignment.clearMarks = MarksToClear(member, Corner::None, true);
        assignment.slot = slot++;
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
            BotPlan const& assignment = plan.bots.at(member->GetGUID());
            bool const rotateEarly = assignment.slot % RAID_ROTATION_STACKS == 0;
            if (assignment.reserved || MarksToClear(member, corner, rotateEarly) ||
                (corner == Corner::Zeliek && (supportSlot >= ZELIEK_SUPPORT_SLOTS || PlayerbotAI::IsMelee(member))))
                continue;
            PlanSupport(member, corner, bosses[index], holders[index], supportSlot++, false, plan);
        }
        break;
    }
}

Creature* Resolve(Player* player, ObjectGuid guid)
{
    Creature* creature = guid ? player->GetMap()->GetCreature(guid) : nullptr;
    return creature && creature->IsAlive() ? creature : nullptr;
}

std::string_view TauntSpell(Player* player)
{
    switch (player->getClass())
    {
        case CLASS_WARRIOR:
            return "taunt";
        case CLASS_DRUID:
            return "growl";
        case CLASS_PALADIN:
            return "hand of reckoning";
        case CLASS_DEATH_KNIGHT:
            return "dark command";
        default:
            return {};
    }
}
}  // namespace

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
    add(Tactic::Taunt, ACTION_RAID + 7);
    add(Tactic::Marks, ACTION_RAID + 6);
    add(Tactic::HolyWrath, ACTION_RAID + 5);
    add(Tactic::Meteor, ACTION_RAID + 4);
    add(Tactic::Corner, ACTION_RAID + 3);
    add(Tactic::Attack, ACTION_RAID + 2);
}

template <Tactic tactic>
bool HorsemenTrigger<tactic>::IsActive()
{
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
    std::string const spell(TauntSpell(bot));
    if (!target || spell.empty())
        return false;

    if (bot->getClass() == CLASS_DRUID && bot->GetShapeshiftForm() != FORM_BEAR &&
        bot->GetShapeshiftForm() != FORM_DIREBEAR)
    {
        if (botAI->CanCastSpell("dire bear form", bot))
            return botAI->CastSpell("dire bear form", bot);
        return botAI->CanCastSpell("bear form", bot) && botAI->CastSpell("bear form", bot);
    }
    if (bot->getClass() == CLASS_PALADIN && !bot->HasAura(SPELL_RIGHTEOUS_FURY) &&
        botAI->CanCastSpell("righteous fury", bot))
        return botAI->CastSpell("righteous fury", bot);

    if (plan->handoff && !bot->IsWithinMeleeRange(target))
        return false;

    return target->GetVictim() != bot && botAI->CanCastSpell(spell, target) && botAI->CastSpell(spell, target);
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
    constexpr MovementPriority priority = (tactic == Tactic::VoidZone || tactic == Tactic::Marks)
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
        if constexpr (tactic == Tactic::Marks)
        {
            EncounterHelpers::StopAttackingAndChanneling(bot);
            if (Pet* pet = bot->GetPet(); pet && pet->IsInCombat())
                EncounterHelpers::CallBackPets(bot);
        }
        if (IsWaitingForLastMove(priority))
            return false;

        if constexpr (tactic == Tactic::Marks)
            destination = WaitPosition(bot, raid->markSources, raid->voidZones, plan->clearMarks, plan->slot);
        else if constexpr (tactic == Tactic::Meteor)
            tolerance = METEOR_SOAK_DISTANCE;
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

                if (plan->handoff ? bot->IsWithinMeleeRange(approach)
                                  : bot->GetExactDist2d(approach) <= APPROACH_DISTANCE)
                    return false;
                destination = approach->GetPosition();
                tolerance = plan->handoff ? HOLY_WRATH_ARRIVAL_TOLERANCE : APPROACH_DISTANCE;
            }
        }
    }

    if (bot->GetExactDist(&destination) <= tolerance)
        return false;

    if constexpr (tactic == Tactic::VoidZone)
        context->GetValue<LastMovement&>("last movement")->Get().clear();
    else
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

    BotPlan const* plan = FindPlan(GetPlan(bot), bot);
    if (!plan)
        return 1.0f;

    if (dynamic_cast<DpsAssistAction*>(action) || dynamic_cast<TankAssistAction*>(action) ||
        dynamic_cast<FollowAction*>(action) || dynamic_cast<CombatFormationMoveAction*>(action) ||
        dynamic_cast<FleeAction*>(action) || EncounterHelpers::IsTauntAction(bot, action))
        return 0.0f;

    if (plan->tank && (dynamic_cast<CastCatFormAction*>(action) || dynamic_cast<CastCasterFormAction*>(action)))
        return 0.0f;

    if (auto* reach = dynamic_cast<ReachTargetAction*>(action))
    {
        Unit* target = reach->GetTarget();
        if (target && (!bot->IsFriendlyTo(target) || plan->movement == Tactic::HolyWrath ||
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
template class HorsemenTrigger<Tactic::Taunt>;
template class HorsemenTrigger<Tactic::Corner>;
template class HorsemenTrigger<Tactic::Marks>;
template class HorsemenTrigger<Tactic::Meteor>;
template class HorsemenTrigger<Tactic::VoidZone>;
template class HorsemenTrigger<Tactic::HolyWrath>;
template class HorsemenTrigger<Tactic::Attack>;

template class HorsemenMoveAction<Tactic::Corner>;
template class HorsemenMoveAction<Tactic::Marks>;
template class HorsemenMoveAction<Tactic::Meteor>;
template class HorsemenMoveAction<Tactic::VoidZone>;
template class HorsemenMoveAction<Tactic::HolyWrath>;
}  // namespace Naxx40Horsemen
