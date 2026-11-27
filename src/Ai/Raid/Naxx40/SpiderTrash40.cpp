/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SpiderTrash40.h"

#include "Ai/Raid/RaidClear/Common/Interrupts.h"
#include "AiObjectContext.h"
#include "CombatManager.h"
#include "Creature.h"
#include "InstanceScript.h"
#include "Naxx40Encounter.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Spell.h"
#include "SpellInfo.h"

namespace
{
    constexpr uint32 NPC_DREAD_CREEPER = 351010;
    constexpr uint32 NPC_CARRION_SPINNER = 351011;
    constexpr uint32 NPC_VENOM_STALKER = 351012;
    constexpr uint32 NPC_INFECTIOUS_SKITTERER = 351013;
    constexpr uint32 NPC_CRYPT_REAVER = 351014;
    constexpr uint32 NPC_NAXXRAMAS_CULTIST = 351016;
    constexpr uint32 NPC_NAXXRAMAS_ACOLYTE = 351017;
    constexpr uint32 SPELL_ARCANE_EXPLOSION = 28450;
    constexpr uint32 SPELL_SHADOW_BOLT_VOLLEY = 28448;

    enum Priority : uint8
    {
        Acolyte,
        VenomStalker,
        LooseReaver,
        Spinner,
        TankedReaver,
        DreadCreeper,
        Cultist,
        Skitterer,
    };

    constexpr float TrashRange = 45.0f;
    constexpr char AcolyteCasting[] = "naxx40 spider acolyte casting";
    constexpr char InterruptAcolyte[] = "naxx40 interrupt spider acolyte";

    bool Between(uint32 value, uint32 first, uint32 last)
    {
        return value >= first && value <= last;
    }

    bool IsSpiderTrashFight(Player const& bot)
    {
        if (!IsNaxx40Map(bot.FindMap()) || !bot.GetGroup() || !bot.IsAlive() || !bot.IsInCombat() ||
            bot.GetPositionX() < 3000.0f || bot.GetPositionX() > 3440.0f ||
            bot.GetPositionY() < -3850.0f || bot.GetPositionY() > -3430.0f)
            return false;

        InstanceScript const* instance = bot.GetInstanceScript();
        return instance && !instance->IsEncounterInProgress();
    }

    bool IsNaxxramasAcolyte(Unit const& enemy)
    {
        Creature const* creature = enemy.ToCreature();
        return creature && creature->GetEntry() == NPC_NAXXRAMAS_ACOLYTE &&
            Between(creature->GetSpawnId(), 361794, 361841);
    }

    bool FightIsWithRaid(Player const& bot, Unit const& enemy)
    {
        for (auto const& [guid, reference] : enemy.GetCombatManager().GetPvECombatRefs())
        {
            if (!guid.IsPlayer() || reference->IsSuppressedFor(&enemy))
                continue;
            Player const* member = reference->GetOther(&enemy)->ToPlayer();
            if (member && member->IsAlive() && member->GetGroup() == bot.GetGroup())
                return true;
        }
        return false;
    }

    bool CanEngage(Player const& bot, Unit const& enemy)
    {
        return enemy.IsAlive() && enemy.IsInCombat() && !enemy.HasUnitState(UNIT_STATE_EVADE) && bot.IsInMap(&enemy) &&
            bot.GetExactDist(&enemy) <= TrashRange && bot.IsValidAttackTarget(&enemy) &&
            bot.IsWithinLOSInMap(&enemy) && FightIsWithRaid(bot, enemy);
    }

    std::optional<uint8> SpawnPriority(Player const& marker, Creature const& enemy)
    {
        uint32 const spawn = enemy.GetSpawnId();
        switch (enemy.GetEntry())
        {
            case NPC_NAXXRAMAS_ACOLYTE:
                if (IsNaxxramasAcolyte(enemy))
                    return Acolyte;
                break;
            case NPC_VENOM_STALKER:
                if (spawn == 361190 || spawn == 361191 || spawn == 361214)
                    return VenomStalker;
                break;
            case NPC_CRYPT_REAVER:
                if (spawn == 361297 || spawn == 361298)
                {
                    Unit* victim = enemy.GetVictim();
                    Player* tank = victim ? victim->ToPlayer() : nullptr;
                    bool const loose = victim && (!tank || tank->GetGroup() != marker.GetGroup() ||
                        !PlayerbotAI::IsTank(tank));
                    return loose ? LooseReaver : TankedReaver;
                }
                break;
            case NPC_CARRION_SPINNER:
                if (Between(spawn, 361187, 361189) || Between(spawn, 361192, 361194) ||
                    Between(spawn, 361211, 361213) || Between(spawn, 361299, 361302) ||
                    Between(spawn, 361877, 361884))
                    return Spinner;
                break;
            case NPC_DREAD_CREEPER:
                if (Between(spawn, 361205, 361210))
                    return DreadCreeper;
                break;
            case NPC_NAXXRAMAS_CULTIST:
                if (Between(spawn, 361794, 361841))
                    return Cultist;
                break;
            case NPC_INFECTIOUS_SKITTERER:
                if (Between(spawn, 361195, 361204) || Between(spawn, 361306, 361309) ||
                    Between(spawn, 361776, 361793) || spawn == 361844 || Between(spawn, 361850, 361858) ||
                    Between(spawn, 361866, 361874) || Between(spawn, 361885, 361904))
                    return Skitterer;
                break;
        }
        return std::nullopt;
    }

    struct AcolyteInterrupt
    {
        Unit* caster = nullptr;
        char const* spell = nullptr;
        bool arcaneExplosion = false;
        float distance = 0.0f;
    };

    AcolyteInterrupt FindAcolyteInterrupt(PlayerbotAI* ai, Player* bot)
    {
        AcolyteInterrupt best;
        for (ObjectGuid const& guid : ai->GetAiObjectContext()->GetValue<GuidVector>("attackers")->RefGet())
        {
            Unit* caster = ai->GetUnit(guid);
            if (!caster || !IsNaxxramasAcolyte(*caster))
                continue;

            Spell const* spell = caster->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            SpellInfo const* info = spell ? spell->GetSpellInfo() : nullptr;
            if (!info || (info->Id != SPELL_ARCANE_EXPLOSION && info->Id != SPELL_SHADOW_BOLT_VOLLEY))
                continue;

            bool const arcaneExplosion = info->Id == SPELL_ARCANE_EXPLOSION;
            float const distance = bot->GetExactDist(caster);
            if (best.caster && ((best.arcaneExplosion && !arcaneExplosion) ||
                (best.arcaneExplosion == arcaneExplosion && distance >= best.distance)))
                continue;
            if (!CanEngage(*bot, *caster) || RaidClear::InterruptJustSent(caster))
                continue;

            if (char const* ready = RaidClear::ReadyStop(ai, bot, caster, info, arcaneExplosion))
                best = {caster, ready, arcaneExplosion, distance};
        }
        return best;
    }

    class AcolyteCastingTrigger final : public Trigger
    {
    public:
        explicit AcolyteCastingTrigger(PlayerbotAI* ai) : Trigger(ai, AcolyteCasting) {}

        bool IsActive() override
        {
            return IsSpiderTrashFight(*bot) && FindAcolyteInterrupt(botAI, bot).caster;
        }
    };

    class InterruptAcolyteAction final : public Action
    {
    public:
        explicit InterruptAcolyteAction(PlayerbotAI* ai) : Action(ai, InterruptAcolyte) {}

        bool Execute(Event /*event*/) override
        {
            if (!IsSpiderTrashFight(*bot))
                return false;

            AcolyteInterrupt const choice = FindAcolyteInterrupt(botAI, bot);
            if (!choice.caster || !botAI->CastSpell(choice.spell, choice.caster))
                return false;

            RaidClear::InterruptSent(choice.caster);
            return true;
        }
    };

    class SpiderTrashStrategy final : public Strategy
    {
    public:
        explicit SpiderTrashStrategy(PlayerbotAI* ai) : Strategy(ai) {}
        std::string const getName() override { return Naxx40SpiderTrashStrategyName; }

        void InitTriggers(std::vector<TriggerNode*>& triggers) override
        {
            Strategy::InitTriggers(triggers);
            triggers.push_back(new TriggerNode(AcolyteCasting, {NextAction(InterruptAcolyte, ACTION_INTERRUPT + 1)}));
        }
    };
}

std::optional<uint8> Naxx40SpiderTrashRank(Player const& marker, Unit const& enemy)
{
    Creature const* creature = enemy.ToCreature();
    if (!creature)
        return std::nullopt;

    auto const rank = SpawnPriority(marker, *creature);
    if (!rank || !IsSpiderTrashFight(marker) || !CanEngage(marker, enemy))
        return std::nullopt;
    return rank;
}

Naxx40SpiderActionContext::Naxx40SpiderActionContext()
{
    creators[InterruptAcolyte] = [](PlayerbotAI* ai) { return new InterruptAcolyteAction(ai); };
}

Naxx40SpiderTriggerContext::Naxx40SpiderTriggerContext()
{
    creators[AcolyteCasting] = [](PlayerbotAI* ai) { return new AcolyteCastingTrigger(ai); };
}

Naxx40SpiderStrategyContext::Naxx40SpiderStrategyContext()
{
    creators[Naxx40SpiderTrashStrategyName] = [](PlayerbotAI* ai) { return new SpiderTrashStrategy(ai); };
}
