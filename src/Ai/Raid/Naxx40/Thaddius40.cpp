/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 *
 * Encounter phases and platform coordinates adapted from electiveDev/playerbots-custom-strategies
 * at 58a5b7b5ed760d2236f894cf9b535d2c18e7bc69 and the mod-playerbots Naxxramas strategy.
 */

#include "Thaddius40.h"
#include "AttackAction.h"
#include "EncounterHelpers.h"
#include "GenericSpellActions.h"
#include "Group.h"
#include "InstanceScript.h"
#include "MotionMaster.h"
#include "Multiplier.h"
#include "Naxx40Encounter.h"
#include "Pet.h"
#include "Player.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "Strategy.h"
#include "../Naxx/NaxxSpellIds.h"
#include <array>
#include <cmath>

namespace
{
constexpr char TacticsAction[] = "thaddius 40 tactics";
constexpr char EncounterTrigger[] = "thaddius 40 active";

constexpr float AddPlatformZ = 312.0f;
constexpr float AddPlatformTolerance = 2.0f;
constexpr float AddPlatformRadius = 30.0f;
constexpr float BossPlatformZ = 304.02f;
Position const Center(3512.19f, -2928.58f, BossPlatformZ);
Position const NeutralRanged(3504.68f, -2936.68f, BossPlatformZ);
// Keep each add within a few yards of its IP spawn and its own Tesla Coil link.
std::array<Position, 2> const TankPositions = {{{3447.45f, -2929.42f, AddPlatformZ},
                                               {3511.14f, -2990.65f, AddPlatformZ}}};
std::array<Position, 2> const RangedPositions = {{{3441.01f, -2942.04f, AddPlatformZ},
                                                 {3500.45f, -2997.92f, AddPlatformZ}}};
std::array<Position, 2> const JumpLedges = {{{3462.99f, -2918.90f, AddPlatformZ},
                                            {3520.65f, -2976.51f, AddPlatformZ}}};
std::array<Position, 2> const JumpLandings = {{{3471.36f, -2910.65f, BossPlatformZ},
                                              {3528.80f, -2967.04f, BossPlatformZ}}};
std::array<Position, 2> const ChargeMelee = {{{3508.29f, -2920.12f, BossPlatformZ},
                                             {3519.74f, -2931.69f, BossPlatformZ}}};
std::array<Position, 2> const ChargeRanged = {{{3503.70f, -2915.49f, BossPlatformZ},
                                              {3524.32f, -2936.26f, BossPlatformZ}}};
// Separate lanes keep bots swapping in opposite directions from running through one another.
std::array<Position, 2> const ChargeCrossings = {{{3508.30f, -2931.58f, BossPlatformZ},
                                                 {3519.70f, -2920.10f, BossPlatformZ}}};

struct Fight
{
    Creature* boss = nullptr;
    std::array<Creature*, 2> adds = {};
};

Creature* FindCreature(InstanceScript* instance, uint32 data, uint32 entry)
{
    Creature* creature = instance->GetCreature(data);
    return creature && creature->GetEntry() == entry && creature->IsInWorld() && creature->IsAlive()
        ? creature : nullptr;
}

bool CanAttack(Unit const* unit)
{
    return unit && !unit->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) && !unit->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE);
}

Fight ReadFight(Player* bot)
{
    if (!IsNaxx40Map(bot->GetMap()) || !bot->IsAlive() || !bot->IsInCombat() || bot->GetExactDist2d(Center) > 110.0f)
        return {};

    InstanceScript* instance = bot->GetInstanceScript();
    if (!instance || instance->GetBossState(Thaddius40::Encounter) != IN_PROGRESS)
        return {};

    return {FindCreature(instance, Thaddius40::ThaddiusData, Thaddius40::ThaddiusEntry),
            {FindCreature(instance, Thaddius40::StalaggData, Thaddius40::StalaggEntry),
             FindCreature(instance, Thaddius40::FeugenData, Thaddius40::FeugenEntry)}};
}

bool IsTank(Unit* unit)
{
    Player* player = unit ? unit->ToPlayer() : nullptr;
    return player && player->IsAlive() && (PlayerbotAI::IsTank(player) || PlayerbotAI::IsExplicitMainTank(player));
}

bool IsOnAddPlatform(Unit const& unit, uint8 side)
{
    return unit.GetPositionZ() >= AddPlatformZ - AddPlatformTolerance &&
        unit.GetExactDist2dSq(TankPositions[side]) <= AddPlatformRadius * AddPlatformRadius;
}

bool HasTankOnSide(Creature const& add, uint8 side)
{
    Unit* victim = add.GetVictim();
    return IsTank(victim) && IsOnAddPlatform(*victim, side);
}

enum class OpeningRole
{
    Healer,
    Ranged,
    Melee,
    ReserveTank
};

OpeningRole RoleBucket(Player* member)
{
    if (PlayerbotAI::IsTank(member))
        return OpeningRole::ReserveTank;
    if (PlayerbotAI::IsHeal(member))
        return OpeningRole::Healer;
    return PlayerbotAI::IsRanged(member) ? OpeningRole::Ranged : OpeningRole::Melee;
}

Player* FindLocalTank(Player* player, Group* group, Player* main)
{
    Player* firstTank = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == main || !member->IsAlive() || !member->IsInMap(player) ||
            !PlayerbotAI::IsTank(member))
            continue;

        if (!main || group->IsAssistant(member->GetGUID()))
            return member;
        if (!firstTank)
            firstTank = member;
    }
    return firstTank;
}

Thaddius40::OpeningAssignment AssignForCombat(Player* bot, Fight const& fight)
{
    Thaddius40::OpeningAssignment assignment = Thaddius40::AssignOpening(bot);
    if (!IsTank(bot))
        return assignment;

    // Zone-wide aggro can list a tank on the far ramp before the tank swap.
    for (uint8 side = 0; side < fight.adds.size(); ++side)
    {
        if (CanAttack(fight.adds[side]) && fight.adds[side]->GetVictim() == bot &&
            IsOnAddPlatform(*bot, side))
            return {side, true};
    }

    uint8 const otherSide = 1 - assignment.side;
    if (assignment.tank && IsOnAddPlatform(*bot, otherSide))
    {
        Creature* assigned = fight.adds[assignment.side];
        Creature* other = fight.adds[otherSide];
        if (CanAttack(assigned) && HasTankOnSide(*assigned, assignment.side) && CanAttack(other) &&
            !HasTankOnSide(*other, otherSide))
            assignment.side = otherSide;
    }
    return assignment;
}

bool HoldDamage(Unit* target, Fight const& fight)
{
    if (!target || (target != fight.adds[0] && target != fight.adds[1]))
        return false;

    if (!CanAttack(target))
        return true;

    Creature* other = target == fight.adds[0] ? fight.adds[1] : fight.adds[0];
    if (!CanAttack(other))
        return false;

    float const health = target->GetHealthPct();
    float const gap = other->GetHealthPct() - health;
    return gap > 12.0f || (health <= 30.0f && gap >= 4.0f);
}

bool StopDamage(Player* bot)
{
    bool stopped = bot->AttackStop();
    for (CurrentSpellTypes slot : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL, CURRENT_AUTOREPEAT_SPELL})
    {
        Spell* spell = bot->GetCurrentSpell(slot);
        if (spell && !spell->GetSpellInfo()->IsPositive())
        {
            bot->InterruptSpell(slot);
            stopped = true;
        }
    }

    if (Pet* pet = bot->GetPet(); pet && pet->GetVictim())
    {
        pet->AttackStop();
        pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, PET_FOLLOW_ANGLE);
        stopped = true;
    }
    return stopped;
}

char const* TauntSpell(Player* bot)
{
    switch (bot->getClass())
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
            return nullptr;
    }
}

int8 Polarity(Unit const* unit)
{
    // The aura applied by Polarity Shift identifies the new side before damage stacks build.
    if (!unit)
        return 0;
    if (unit->HasAura(NaxxSpellIds::NegativeCharge10))
        return -1;
    if (unit->HasAura(NaxxSpellIds::PositiveCharge10))
        return 1;
    return 0;
}

class Thaddius40Action final : public AttackAction
{
public:
    explicit Thaddius40Action(PlayerbotAI* botAI) : AttackAction(botAI, TacticsAction) {}

    bool Execute(Event /*event*/) override
    {
        Fight const fight = ReadFight(bot);
        if (!fight.boss)
            return false;
        if (CanAttack(fight.boss))
            return HandleBoss(fight.boss);

        lastPolarity = 0;
        if (CanAttack(fight.adds[0]) || CanAttack(fight.adds[1]))
            return HandleAdds(fight);

        bool const stopped = StopDamage(bot);
        return ReachPlatform(Center) || stopped;
    }

private:
    bool MoveToPosition(Position const& position, float radius = 2.0f)
    {
        if (bot->GetExactDist2dSq(position) <= radius * radius &&
            std::abs(bot->GetPositionZ() - position.GetPositionZ()) < 2.0f)
            return false;

        return MoveTo(bot->GetMapId(), position.GetPositionX(), position.GetPositionY(), position.GetPositionZ(),
                      false, false, true, false, MovementPriority::MOVEMENT_COMBAT);
    }

    bool HandleAdds(Fight const& fight)
    {
        Thaddius40::OpeningAssignment const assignment = AssignForCombat(bot, fight);
        Creature* add = fight.adds[assignment.side];
        if (!CanAttack(add))
            return StopDamage(bot);

        bool const ranged = PlayerbotAI::IsRanged(bot) || PlayerbotAI::IsHeal(bot);
        bool const tankOnSide = HasTankOnSide(*add, assignment.side);
        bool const hold = HoldDamage(add, fight) || (!assignment.tank && !tankOnSide);
        Unit* victim = bot->GetVictim();
        bool const stopped = (hold || (victim && victim != add)) && StopDamage(bot);

        if (assignment.tank)
        {
            Unit* addVictim = add->GetVictim();
            if (addVictim == bot && MoveToPosition(TankPositions[assignment.side], 3.0f))
                return true;
            if (addVictim != bot && !tankOnSide)
            {
                if (!IsOnAddPlatform(*bot, assignment.side))
                    return MoveToPosition(TankPositions[assignment.side], 3.0f) || stopped;

                if (addVictim && bot->IsWithinLOSInMap(add))
                    if (char const* taunt = TauntSpell(bot); taunt && botAI->CanCastSpell(taunt, add) &&
                        botAI->CastSpell(taunt, add))
                        return true;

                if (!bot->IsWithinMeleeRange(add))
                    return MoveNear(add, 1.0f, MovementPriority::MOVEMENT_COMBAT) || stopped;
                // A tank missing threat must be able to reach and claim the add even during a damage hold.
                if (bot->GetVictim() != add)
                    return Attack(add) || stopped;
            }
        }
        else if (ranged && MoveToPosition(RangedPositions[assignment.side], 4.0f))
            return true;

        if (hold)
            return stopped;

        if (!ranged && !bot->IsWithinMeleeRange(add))
            return MoveNear(add, 1.0f, MovementPriority::MOVEMENT_COMBAT) || stopped;

        if (AI_VALUE(Unit*, "current target") != add || bot->GetVictim() != add)
            return Attack(add) || stopped;

        return stopped;
    }

    bool ReachPlatform(Position const& destination)
    {
        if (bot->GetPositionZ() < AddPlatformZ - 3.0f)
            return MoveToPosition(destination);

        uint8 const side = bot->GetExactDist2dSq(JumpLedges[0]) <= bot->GetExactDist2dSq(JumpLedges[1]) ? 0 : 1;
        if (bot->GetExactDist2dSq(JumpLedges[side]) > 2.25f)
            return MoveToPosition(JumpLedges[side], 1.5f);

        Position const& landing = JumpLandings[side];
        return JumpTo(bot->GetMapId(), landing.GetPositionX(), landing.GetPositionY(), landing.GetPositionZ(),
                      MovementPriority::MOVEMENT_COMBAT);
    }

    bool HandleBoss(Creature* boss)
    {
        int8 const polarity = Polarity(bot);
        if (polarity != lastPolarity && botAI->CanMove())
        {
            AI_VALUE(LastMovement&, "last movement").clear();
            lastPolarity = polarity;
        }

        bool const ranged = !IsTank(bot) && (PlayerbotAI::IsRanged(bot) || PlayerbotAI::IsHeal(bot));
        Position const* destination = ranged || Polarity(boss->GetVictim()) ? &NeutralRanged : &Center;
        if (polarity)
        {
            uint8 const side = polarity > 0 ? 1 : 0;
            destination = ranged ? &ChargeRanged[side] : &ChargeMelee[side];

            // A bot starting across the room takes its own lane around the boss before stacking.
            Position const& crossing = ChargeCrossings[side];
            float const ownDistance = bot->GetExactDist2dSq(ChargeMelee[side]);
            float const oppositeDistance = bot->GetExactDist2dSq(ChargeMelee[1 - side]);
            if (oppositeDistance + 18.0f < ownDistance && bot->GetExactDist2dSq(crossing) > 6.25f)
                destination = &crossing;
        }

        if (bot->GetExactDist2dSq(*destination) > 4.0f ||
            std::abs(bot->GetPositionZ() - BossPlatformZ) >= 2.0f)
        {
            if (bot->HasUnitState(UNIT_STATE_CASTING) && botAI->CanMove())
                bot->InterruptNonMeleeSpells(false, 0, false);
            return ReachPlatform(*destination);
        }

        if (AI_VALUE(Unit*, "current target") != boss || bot->GetVictim() != boss)
            return Attack(boss);
        return false;
    }

    int8 lastPolarity = 0;
};

class Thaddius40Trigger final : public Trigger
{
public:
    explicit Thaddius40Trigger(PlayerbotAI* botAI) : Trigger(botAI, EncounterTrigger) {}
    bool IsActive() override { return ReadFight(bot).boss != nullptr; }
};

class Thaddius40Multiplier final : public Multiplier
{
public:
    explicit Thaddius40Multiplier(PlayerbotAI* botAI) : Multiplier(botAI, "thaddius 40 control") {}

    float GetValue(Action* action) override
    {
        Fight const fight = ReadFight(bot);
        if (!fight.boss || action->getName() == TacticsAction)
            return 1.0f;

        bool const melee = dynamic_cast<MeleeAction*>(action) != nullptr;
        if (dynamic_cast<MovementAction*>(action) && !melee)
            return 0.0f;

        if (CanAttack(fight.boss))
            return 1.0f;

        if (EncounterHelpers::IsTauntAction(bot, action) || EncounterHelpers::IsAoeThreatAction(bot, action))
            return 0.0f;

        if (dynamic_cast<CastHealingSpellAction*>(action))
            return 1.0f;

        Unit* target = action->GetTarget();
        auto* cast = dynamic_cast<CastSpellAction*>(action);
        SpellInfo const* spell = cast
            ? sSpellMgr->GetSpellInfo(AI_VALUE2(uint32, "spell id", cast->getSpell())) : nullptr;
        bool const offensive = melee || (target && bot->IsValidAttackTarget(target)) || (spell && !spell->IsPositive());
        if (!offensive)
            return 1.0f;

        Unit* current = AI_VALUE(Unit*, "current target");
        if ((current != fight.adds[0] && current != fight.adds[1]) || !CanAttack(current) || HoldDamage(current, fight))
            return 0.0f;

        if (target && target != current && bot->IsValidAttackTarget(target))
            return 0.0f;

        uint8 const side = current == fight.adds[0] ? 0 : 1;
        if (!IsOnAddPlatform(*bot, side))
            return 0.0f;
        return IsTank(bot) || HasTankOnSide(*fight.adds[side], side) ? 1.0f : 0.0f;
    }
};
}

Thaddius40::OpeningAssignment Thaddius40::AssignOpening(Player* player)
{
    Group* group = player->GetGroup();
    if (!group)
    {
        uint8 const side = player->GetExactDist2dSq(TankPositions[0]) > player->GetExactDist2dSq(TankPositions[1]);
        return {side, IsTank(player)};
    }

    Player* main = EncounterHelpers::GetGroupMainTank(player);
    if (!main || !main->IsInMap(player))
        main = FindLocalTank(player, group, nullptr);
    if (main == player)
        return {0, true};

    Player* assist = main ? FindLocalTank(player, group, main) : nullptr;
    if (assist == player)
        return {1, true};

    // Keep dead members in the order so a death does not shift the rest of their role.
    OpeningRole const role = RoleBucket(player);
    uint8 side = role == OpeningRole::Ranged || role == OpeningRole::ReserveTank;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member == player)
            break;
        if (member && member != main && member != assist && member->IsInMap(player) && RoleBucket(member) == role)
            side = 1 - side;
    }
    return {side, false};
}

Thaddius40ActionContext::Thaddius40ActionContext()
{
    creators[TacticsAction] = [](PlayerbotAI* botAI) { return new Thaddius40Action(botAI); };
}

Thaddius40TriggerContext::Thaddius40TriggerContext()
{
    creators[EncounterTrigger] = [](PlayerbotAI* botAI) { return new Thaddius40Trigger(botAI); };
}

void AddThaddius40Triggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(EncounterTrigger, {NextAction(TacticsAction, ACTION_RAID + 4)}));
}

void AddThaddius40Multipliers(PlayerbotAI* botAI, std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new Thaddius40Multiplier(botAI));
}
