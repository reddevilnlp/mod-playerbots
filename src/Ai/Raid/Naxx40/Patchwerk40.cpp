/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Patchwerk40.h"

#include "CombatManager.h"
#include "Creature.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Multiplier.h"
#include "Naxx40Encounter.h"
#include "ObjectAccessor.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "Strategy.h"
#include "ThreatManager.h"
#include "Trigger.h"
#include "UseItemAction.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <string_view>

namespace
{
constexpr char HealTrigger[] = "naxx40 patchwerk healer";
constexpr char HealAction[] = "naxx40 patchwerk heal assigned tank";
constexpr char AssignmentValue[] = "naxx40 patchwerk healer assignment";
constexpr char TimingValue[] = "naxx40 patchwerk healer timing";
constexpr char SuppliesTrigger[] = "naxx40 patchwerk tank consumable needed";
constexpr char SuppliesAction[] = "naxx40 patchwerk use tank consumable";
constexpr char SuppliesValue[] = "naxx40 patchwerk tank preparation";
constexpr uint32 PatchwerkData = 100;
constexpr uint32 AssignmentIntervalMs = 2000;
constexpr uint32 OpeningStaggerMs = 600;
constexpr uint32 RefreshWindowMs = 2200;
constexpr uint32 PendingHealWindowMs = 450;
constexpr uint32 OpeningMs = 8000;
constexpr uint32 WeakenedSoul = 6788;
constexpr float HealerCampRadius = 1.5f;
constexpr float TankCampRadius = 5.0f;
constexpr float PrepBossRange = 48.0f;
constexpr float OpeningBossRange = 26.0f;
constexpr float PrepMinimumMana = 60.0f;
constexpr float HealRange = 38.0f;
constexpr float EmergencyHealth = 55.0f;
constexpr float SustainHealth = 85.0f;
constexpr float PrecastMana = 35.0f;
constexpr uint8 SupportHealersPerTank = 4;
constexpr int32 StoneshieldRefreshMs = 8000;
constexpr uint32 PreparationLeaseMs = 4000;
constexpr uint32 ShotLeaseMs = 8000;
constexpr uint32 ItemRetryMs = 1000;
constexpr uint32 FoodWaitMs = 11500;
constexpr std::array HealingClasses = {CLASS_DRUID, CLASS_PRIEST, CLASS_SHAMAN, CLASS_PALADIN};

struct Assignment
{
    ObjectGuid tank;
    uint8 slot = 0;
};

struct Timing
{
    ObjectGuid encounter;
    uint32 instanceId = 0;
    uint32 openingAt = 0;
    bool mainTankEngaged = false;
    bool loggedPrep = false;
    bool loggedDirect = false;
};

struct TankPreparation
{
    Patchwerk40TankPreparation phase = Patchwerk40TankPreparation::Idle;
    ObjectGuid encounter;
    ObjectGuid group;
    uint32 publishedAt = 0;
    uint32 stoneshieldReadyAt = 0;
    uint32 lastItemAttempt = 0;
    bool fightStarted = false;
};

bool IsHeadlessBot(Player const* player)
{
    return player && player->GetSession() && player->GetSession()->IsHeadless();
}

bool IsHealingBot(Player* player)
{
    return IsHeadlessBot(player) && player->IsAlive() && GET_PLAYERBOT_AI(player) && PlayerbotAI::IsHeal(player);
}

Creature* GetPatchwerk(Player* player)
{
    if (!IsNaxx40Map(player->FindMap()))
        return nullptr;
    InstanceScript* const instance = player->GetInstanceScript();
    Creature* const boss = instance ? instance->GetCreature(PatchwerkData) : nullptr;
    return boss && boss->GetEntry() == Naxx40PatchwerkEntry && boss->IsAlive() ? boss : nullptr;
}

bool CanPrepare(Player const& healer, Creature const& boss)
{
    return !healer.IsInCombat() && !boss.IsInCombat() &&
           healer.GetInstanceScript()->GetBossState(Naxx40PatchwerkEncounter) == NOT_STARTED &&
           healer.GetExactDist(&Patchwerk40Position::RaidCamp) <= HealerCampRadius &&
           boss.GetExactDist(&Patchwerk40Position::PullCamp) <= PrepBossRange &&
           healer.GetPowerPct(POWER_MANA) >= PrepMinimumMana;
}

bool InPatchwerkFight(Player const& player, Creature const& boss)
{
    return player.IsInCombat() && boss.IsInCombat() && player.GetCombatManager().IsInCombatWith(&boss) &&
           player.GetInstanceScript()->GetBossState(Naxx40PatchwerkEncounter) == IN_PROGRESS;
}

TankPreparation& TankSupplies(PlayerbotAI& ai)
{
    return ai.GetAiObjectContext()->GetValue<TankPreparation>(SuppliesValue)->RefGet();
}

bool IsStoneshieldReserved(Player& tank, Creature const& boss, TankPreparation& preparation)
{
    using namespace Patchwerk40TankSupplies;
    if (!InPatchwerkFight(tank, boss))
    {
        if (preparation.fightStarted && !boss.IsInCombat())
            preparation = {};
        return false;
    }

    if (!preparation.fightStarted && preparation.stoneshieldReadyAt &&
        getMSTimeDiff(preparation.stoneshieldReadyAt, getMSTime()) <= ShotLeaseMs)
        preparation.fightStarted = true;
    return preparation.fightStarted && !tank.GetLastPotionId() && tank.HasItemCount(StoneshieldItem, 1);
}

uint32 NeededTankItem(PlayerbotAI& ai)
{
    using namespace Patchwerk40TankSupplies;
    Player* const bot = ai.GetBot();
    if (!IsHeadlessBot(bot) || !bot->IsAlive() || !bot->GetGroup())
        return 0;

    TankPreparation& preparation = TankSupplies(ai);
    if (preparation.phase == Patchwerk40TankPreparation::Idle ||
        preparation.group != bot->GetGroup()->GetGUID())
        return 0;

    Creature const* const boss = GetPatchwerk(bot);
    if (!boss || preparation.encounter != boss->GetGUID())
    {
        preparation = {};
        return 0;
    }

    if (boss->IsInCombat())
    {
        if (!IsStoneshieldReserved(*bot, *boss, preparation))
            return 0;
        return !HasTimeRemaining(*bot, StoneshieldAura, StoneshieldRefreshMs) ? StoneshieldItem : 0;
    }

    if (preparation.fightStarted || getMSTimeDiff(preparation.publishedAt, getMSTime()) > PreparationLeaseMs)
    {
        preparation = {};
        return 0;
    }
    if (bot->IsInCombat() || bot->GetInstanceScript()->GetBossState(Naxx40PatchwerkEncounter) != NOT_STARTED)
        return 0;

    if (preparation.phase == Patchwerk40TankPreparation::Shot)
        return !HasTimeRemaining(*bot, StoneshieldAura, MinimumStoneshieldMs) ? StoneshieldItem : 0;

    if (!HasTimeRemaining(*bot, FlaskAura, MinimumFlaskMs) && bot->HasItemCount(FlaskItem, 1))
        return FlaskItem;
    if (!HasTimeRemaining(*bot, FoodAura, MinimumFoodMs) && !bot->HasAura(EatingAura))
        return FoodItem;
    return 0;
}

class PatchwerkSuppliesTrigger final : public Trigger
{
public:
    explicit PatchwerkSuppliesTrigger(PlayerbotAI* ai) : Trigger(ai, SuppliesTrigger, 250) {}

    bool IsActive() override
    {
        if (!IsHeadlessBot(bot) || !bot->IsAlive())
            return false;

        TankPreparation const& preparation = TankSupplies(*botAI);
        return preparation.phase != Patchwerk40TankPreparation::Idle;
    }
};

class PatchwerkUseSuppliesAction final : public UseItemAction
{
public:
    explicit PatchwerkUseSuppliesAction(PlayerbotAI* ai) : UseItemAction(ai, SuppliesAction, true) {}

    bool isPossible() override
    {
        return !bot->isMoving() && !bot->IsMounted() && !bot->IsNonMeleeSpellCast(false) &&
               !bot->HasUnitState(UNIT_STATE_LOST_CONTROL);
    }

    bool Execute(Event /*event*/) override
    {
        uint32 const itemId = NeededTankItem(*botAI);
        if (!itemId)
            return false;

        TankPreparation& preparation = TankSupplies(*botAI);
        if (preparation.lastItemAttempt && getMSTimeDiff(preparation.lastItemAttempt, getMSTime()) < ItemRetryMs)
            return false;

        Item* const item = bot->GetItemByEntry(itemId);
        if (!item)
            return false;

        uint32 const spell = item->GetTemplate()->Spells[0].SpellId;
        if (bot->HasSpellCooldown(spell))
            return false;

        preparation.lastItemAttempt = getMSTime();
        SetDuration(0);
        if (!UseItemAuto(item))
            return false;

        if (itemId == Patchwerk40TankSupplies::FoodItem)
            SetDuration(FoodWaitMs);
        return true;
    }
};

class PatchwerkPotionMultiplier final : public Multiplier
{
public:
    explicit PatchwerkPotionMultiplier(PlayerbotAI* ai) : Multiplier(ai, "naxx40 patchwerk reserve potion") {}

    float GetValue(Action* action) override
    {
        if (!dynamic_cast<UseHealingPotion*>(action) && !dynamic_cast<UseManaPotion*>(action))
            return 1.0f;
        if (!IsHeadlessBot(bot) || !bot->IsInCombat() || !bot->GetGroup())
            return 1.0f;

        TankPreparation& preparation = TankSupplies(*botAI);
        if (preparation.phase != Patchwerk40TankPreparation::Shot ||
            preparation.group != bot->GetGroup()->GetGUID())
            return 1.0f;

        Creature const* const boss = GetPatchwerk(bot);
        return boss && preparation.encounter == boss->GetGUID() && IsStoneshieldReserved(*bot, *boss, preparation)
            ? 0.0f : 1.0f;
    }
};

class PatchwerkAssignmentValue final : public CalculatedValue<Assignment>
{
public:
    explicit PatchwerkAssignmentValue(PlayerbotAI* ai)
        : CalculatedValue<Assignment>(ai, AssignmentValue, AssignmentIntervalMs)
    {
    }

    Assignment Calculate() override
    {
        Player* const healer = botAI->GetBot();
        Group* const group = healer->GetGroup();
        if (!group)
            return {};

        std::array<Player*, 2> tanks{};
        std::array<std::vector<ObjectGuid>, HealingClasses.size()> healers;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* const member = ref->GetSource();
            if (!member || !member->IsInMap(healer))
                continue;
            // Keep assignments stable when a tank dies or is resurrected.
            for (std::size_t index = 0; index < tanks.size(); ++index)
                if (!tanks[index] && PlayerbotAI::IsAssistTankOfIndex(member, static_cast<uint8>(index)))
                    tanks[index] = member;
            auto const role = std::find(HealingClasses.begin(), HealingClasses.end(), member->getClass());
            if (role != HealingClasses.end() && member->GetSession() && member->GetSession()->IsHeadless() &&
                GET_PLAYERBOT_AI(member) && PlayerbotAI::IsHeal(member))
                healers[role - HealingClasses.begin()].push_back(member->GetGUID());
        }

        // Send one healer of each class to each tank before filling gaps with the remaining healers.
        for (auto& members : healers)
            std::sort(members.begin(), members.end());
        std::array<uint8, 2> assigned{};
        std::size_t nextTank = 0;
        for (std::size_t pass = 0; pass < SupportHealersPerTank; ++pass)
            for (auto const& members : healers)
                for (std::size_t offset = 0; offset < tanks.size(); ++offset)
                {
                    std::size_t const member = pass * tanks.size() + offset;
                    if (member >= members.size())
                        break;
                    std::size_t index = nextTank++ % tanks.size();
                    if (!tanks[index] || assigned[index] >= SupportHealersPerTank)
                        index = (index + 1) % tanks.size();
                    if (!tanks[index] || assigned[index] >= SupportHealersPerTank)
                        continue;
                    uint8 const slot = assigned[index]++;
                    if (members[member] == healer->GetGUID())
                        return {tanks[index]->GetGUID(), slot};
                }
        return {};
    }
};

struct HealingSpells
{
    std::array<char const*, 2> maintenance;
    std::array<char const*, 3> urgent;
    char const* direct;
};

HealingSpells SpellsFor(Player const& healer)
{
    switch (healer.getClass())
    {
        case CLASS_DRUID:
            return {{"rejuvenation", "regrowth"}, {"swiftmend", "regrowth", "healing touch"}, "healing touch"};
        case CLASS_PRIEST:
            return {{"renew", "power word: shield"}, {"power word: shield", "penance", "flash heal"}, "greater heal"};
        case CLASS_SHAMAN:
            return {{"riptide", nullptr}, {"riptide", "lesser healing wave", nullptr}, "healing wave"};
        case CLASS_PALADIN:
            return {{nullptr, nullptr}, {"holy shock", "flash of light", nullptr}, "holy light"};
        default:
            return {};
    }
}

class PatchwerkHealTrigger final : public Trigger
{
public:
    explicit PatchwerkHealTrigger(PlayerbotAI* ai) : Trigger(ai, HealTrigger, 200) {}

    bool IsActive() override
    {
        if (!IsHealingBot(bot))
            return false;
        Creature const* const boss = GetPatchwerk(bot);
        if (!boss)
        {
            context->GetValue<Timing>(TimingValue)->Reset();
            return false;
        }
        if (CanPrepare(*bot, *boss) || InPatchwerkFight(*bot, *boss))
            return true;
        context->GetValue<Timing>(TimingValue)->Reset();
        return false;
    }
};

class PatchwerkHealAction final : public Action
{
public:
    explicit PatchwerkHealAction(PlayerbotAI* ai) : Action(ai, HealAction) {}

    bool isUseful() override
    {
        return IsHealingBot(bot) && !bot->isMoving() && !bot->IsNonMeleeSpellCast(true) && GetPatchwerk(bot);
    }

    bool Execute(Event /*event*/) override
    {
        Creature* const boss = IsHealingBot(bot) ? GetPatchwerk(bot) : nullptr;
        if (!boss)
            return false;
        bool const preparing = CanPrepare(*bot, *boss);
        if (!preparing && !InPatchwerkFight(*bot, *boss))
            return false;

        Assignment const assignment = AI_VALUE(Assignment, AssignmentValue);
        Player* const tank = ObjectAccessor::GetPlayer(*bot, assignment.tank);
        if (!tank || !tank->IsAlive() || !tank->IsInSameRaidWith(bot) ||
            !PlayerbotAI::IsTank(tank) || !bot->IsWithinDistInMap(tank, HealRange) || !bot->IsWithinLOSInMap(tank))
            return false;

        Timing& timing = context->GetValue<Timing>(TimingValue)->RefGet();
        if (timing.instanceId != bot->GetInstanceId() || timing.encounter != boss->GetGUID())
            timing = {boss->GetGUID(), bot->GetInstanceId()};
        HealingSpells const spells = SpellsFor(*bot);

        if (preparing)
        {
            timing.openingAt = 0;
            timing.mainTankEngaged = false;
            timing.loggedDirect = false;
            if (tank->IsInCombat() || tank->GetExactDist(&Patchwerk40Position::PullCamp) > TankCampRadius)
                return false;
            return Maintain(*tank, spells, timing, true);
        }

        if (!timing.mainTankEngaged)
        {
            Group* const group = bot->GetGroup();
            Unit* const victim = boss->GetVictim();
            if (!group || !victim || victim->GetGUID() != PlayerbotAI::GetMainTankGuid(group) ||
                boss->GetThreatMgr().GetThreat(victim) <= 0.0f)
                return false;
            timing.mainTankEngaged = true;
        }

        return HealTank(*boss, *tank, spells, assignment.slot, timing);
    }

private:
    uint32 KnownSpell(char const* name)
    {
        if (!name)
            return 0;
        uint32 const spell = AI_VALUE2(uint32, "spell id", name);
        return spell && bot->HasSpell(spell) ? spell : 0;
    }

    bool CastOn(Player& target, uint32 spell)
    {
        return spell && botAI->CanCastSpell(spell, &target) && botAI->CastSpell(spell, &target);
    }

    bool NeedsAura(Player& target, uint32 spell, char const* name)
    {
        std::string_view const spellName(name);
        if (spellName == "power word: shield")
            return !target.HasAura(WeakenedSoul) && !target.GetAuraOfRankedSpell(spell);
        Aura const* const aura = target.GetAuraOfRankedSpell(spell, bot->GetGUID());
        return !aura || aura->GetDuration() <= static_cast<int32>(RefreshWindowMs);
    }

    bool Maintain(Player& tank, HealingSpells const& spells, Timing& timing, bool preparing)
    {
        for (char const* name : spells.maintenance)
        {
            uint32 const spell = KnownSpell(name);
            if (!spell || !NeedsAura(tank, spell, name) || !CastOn(tank, spell))
                continue;
            if (preparing && !timing.loggedPrep)
            {
                LOG_INFO("playerbots", "[Naxx40] Patchwerk prep: {} -> {} ({})", bot->GetName(), tank.GetName(), name);
                timing.loggedPrep = true;
            }
            return true;
        }
        return false;
    }

    bool HealTank(Creature const& boss, Player& tank, HealingSpells const& spells, uint8 slot, Timing& timing)
    {
        uint32 const now = getMSTime();
        float const health = tank.GetHealthPct();
        if (!timing.openingAt)
        {
            // Time the first hard casts for the boss's arrival, or damage already on the tank.
            if (health >= SustainHealth && boss.GetExactDist(&tank) > OpeningBossRange)
                return false;
            timing.openingAt = now;
        }

        if (health < EmergencyHealth)
        {
            if (CastUrgent(tank, spells))
                return RecordDirect(tank, timing);
        }
        else
        {
            if (getMSTimeDiff(timing.openingAt, now) < slot * OpeningStaggerMs)
                return false;
            if (health >= SustainHealth && Maintain(tank, spells, timing, false))
                return true;
        }

        uint32 const direct = KnownSpell(spells.direct);
        if (health >= SustainHealth)
        {
            bool const opening = getMSTimeDiff(timing.openingAt, now) < OpeningMs;
            if ((!opening && slot > 0) || bot->GetPowerPct(POWER_MANA) < PrecastMana)
                return false;
        }

        if (health >= EmergencyHealth && WouldOverlap(tank, direct))
            return false;
        if (CastOn(tank, direct))
            return RecordDirect(tank, timing);
        return false;
    }

    bool WouldOverlap(Player const& tank, uint32 spell)
    {
        SpellInfo const* const spellInfo = sSpellMgr->GetSpellInfo(spell);
        if (!spellInfo)
            return false;
        int32 const castTime = spellInfo->CalcCastTime(bot);
        Group* const group = bot->GetGroup();
        for (GroupReference* ref = group ? group->GetFirstMember() : nullptr; ref; ref = ref->next())
        {
            Player const* const member = ref->GetSource();
            if (!member || member == bot || !member->IsInMap(bot))
                continue;
            Spell* const incoming = member->GetCurrentSpell(CURRENT_GENERIC_SPELL);
            if (incoming && incoming->m_targets.GetUnitTargetGUID() == tank.GetGUID() &&
                incoming->GetSpellInfo()->HasEffect(SPELL_EFFECT_HEAL) &&
                std::abs(incoming->GetCastTimeRemaining() - castTime) < static_cast<int32>(PendingHealWindowMs))
                return true;
        }
        return false;
    }

    bool CastUrgent(Player& tank, HealingSpells const& spells)
    {
        for (char const* name : spells.urgent)
        {
            uint32 const spell = KnownSpell(name);
            if (!spell || (std::string_view(name) == "power word: shield" && !NeedsAura(tank, spell, name)))
                continue;
            if (CastOn(tank, spell))
                return true;
        }
        return false;
    }

    bool RecordDirect(Player const& tank, Timing& timing)
    {
        if (!timing.loggedDirect)
        {
            LOG_INFO("playerbots", "[Naxx40] Patchwerk dedicated healing: {} -> {}", bot->GetName(), tank.GetName());
            timing.loggedDirect = true;
        }
        return true;
    }
};
}  // namespace

bool Patchwerk40TankSupplies::HasTimeRemaining(Player const& player, uint32 auraId, int32 minimumMs)
{
    Aura const* const aura = player.GetAura(auraId);
    return aura && (aura->IsPermanent() || aura->GetDuration() >= minimumMs);
}

void SetPatchwerk40TankPreparation(PlayerbotAI& ai, Patchwerk40TankPreparation phase)
{
    Player* const bot = ai.GetBot();
    if (!IsHeadlessBot(bot) || !bot->GetGroup())
        return;

    TankPreparation& preparation = TankSupplies(ai);
    if (phase == Patchwerk40TankPreparation::Idle)
    {
        preparation = {};
        return;
    }

    Creature const* const boss = GetPatchwerk(bot);
    if (!boss || !bot->IsAlive() || bot->IsInCombat() || boss->IsInCombat())
        return;

    ObjectGuid const group = bot->GetGroup()->GetGUID();
    if (preparation.fightStarted || preparation.encounter != boss->GetGUID() || preparation.group != group)
        preparation = {};
    preparation.phase = phase;
    preparation.encounter = boss->GetGUID();
    preparation.group = group;
    preparation.publishedAt = getMSTime();
    using namespace Patchwerk40TankSupplies;
    if (phase == Patchwerk40TankPreparation::Shot && HasTimeRemaining(*bot, StoneshieldAura, MinimumStoneshieldMs))
        preparation.stoneshieldReadyAt = preparation.publishedAt;
    else
        preparation.stoneshieldReadyAt = 0;
}

void AddPatchwerk40Triggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode(HealTrigger, {NextAction(HealAction, ACTION_RAID - 1)}));
    triggers.push_back(new TriggerNode(SuppliesTrigger, {NextAction(SuppliesAction, ACTION_RAID + 1)}));
}

void AddPatchwerk40Multipliers(PlayerbotAI* ai, std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new PatchwerkPotionMultiplier(ai));
}

Patchwerk40ActionContext::Patchwerk40ActionContext()
{
    creators[HealAction] = [](PlayerbotAI* ai) { return new PatchwerkHealAction(ai); };
    creators[SuppliesAction] = [](PlayerbotAI* ai) { return new PatchwerkUseSuppliesAction(ai); };
}

Patchwerk40TriggerContext::Patchwerk40TriggerContext()
{
    creators[HealTrigger] = [](PlayerbotAI* ai) { return new PatchwerkHealTrigger(ai); };
    creators[SuppliesTrigger] = [](PlayerbotAI* ai) { return new PatchwerkSuppliesTrigger(ai); };
}

Patchwerk40ValueContext::Patchwerk40ValueContext()
{
    creators[AssignmentValue] = [](PlayerbotAI* ai) { return new PatchwerkAssignmentValue(ai); };
    creators[TimingValue] = [](PlayerbotAI* ai) { return new ManualSetValue<Timing>(ai, {}, TimingValue); };
    creators[SuppliesValue] = [](PlayerbotAI* ai) { return new ManualSetValue<TankPreparation>(ai, {}, SuppliesValue); };
}
