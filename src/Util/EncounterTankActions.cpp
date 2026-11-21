/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EncounterTankActions.h"

#include <array>
#include <string_view>
#include <utility>

#include "GenericSpellActions.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

namespace EncounterTankActions
{
namespace
{
enum class StateKind
{
    Form,
    ThreatAura,
    Presence
};

struct TankSpells
{
    uint32 taunt;
    std::array<uint32, 2> preparation;
    StateKind kind;
};

constexpr std::array<TankSpells, 4> TANK_SPELLS = {{
    {355, {71, 0}, StateKind::Form},             // Taunt and Defensive Stance
    {6795, {9634, 5487}, StateKind::Form},       // Growl and Dire Bear Form or Bear Form
    {62124, {25780, 0}, StateKind::ThreatAura},  // Hand of Reckoning and Righteous Fury
    {56222, {48263, 0}, StateKind::Presence},    // Dark Command and Frost Presence
}};
constexpr std::array<uint32, 3> PRESENCES = {48263, 48266, 48265};
std::string const STATE_KEY = "playerbots.encounter.tank-state";

struct TankState : DataMap::Base
{
    uint32 original = 0;
    uint32 applied = 0;
    StateKind kind = StateKind::Form;
};

TankSpells const* FindTankSpells(Player const* player)
{
    for (TankSpells const& spells : TANK_SPELLS)
        if (player->HasSpell(spells.taunt))
            return &spells;
    return nullptr;
}

uint32 GetSpellForm(SpellInfo const& spell)
{
    for (SpellEffectInfo const& effect : spell.Effects)
        if (effect.IsAura(SPELL_AURA_MOD_SHAPESHIFT))
            return static_cast<uint32>(effect.MiscValue);
    return FORM_NONE;
}

uint32 GetActiveFormSpell(Player const* player)
{
    for (AuraEffect const* effect : player->GetAuraEffectsByType(SPELL_AURA_MOD_SHAPESHIFT))
        if (static_cast<uint32>(effect->GetMiscValue()) == player->GetShapeshiftForm())
            return effect->GetId();
    return 0;
}

uint32 GetActivePresence(Player const* player)
{
    for (uint32 spell : PRESENCES)
        if (player->HasAura(spell))
            return spell;
    return 0;
}

uint32 FindPreparation(Player const* player, TankSpells const& spells)
{
    SpellInfo const* taunt = sSpellMgr->GetSpellInfo(spells.taunt);
    if (!taunt)
        return 0;

    for (uint32 spell : spells.preparation)
    {
        if (!spell || !player->HasSpell(spell))
            continue;

        if (spells.kind != StateKind::Form)
            return spell;

        SpellInfo const* preparation = sSpellMgr->GetSpellInfo(spell);
        if (preparation && taunt->CheckShapeshift(GetSpellForm(*preparation)) == SPELL_CAST_OK)
            return spell;
    }
    return 0;
}

bool IsPrepared(Player const* player, TankSpells const& spells, uint32 preparation)
{
    if (spells.kind == StateKind::Form)
    {
        SpellInfo const* taunt = sSpellMgr->GetSpellInfo(spells.taunt);
        return taunt && taunt->CheckShapeshift(player->GetShapeshiftForm()) == SPELL_CAST_OK;
    }
    return !preparation || player->HasAura(preparation);
}

uint32 GetCurrentStateSpell(Player const* player, TankState const& state)
{
    switch (state.kind)
    {
        case StateKind::Form:
            return GetActiveFormSpell(player);
        case StateKind::Presence:
            return GetActivePresence(player);
        case StateKind::ThreatAura:
            return player->HasAura(state.applied, player->GetGUID()) ? state.applied : 0;
    }
    return 0;
}

bool CastOnSelf(PlayerbotAI* botAI, uint32 spell)
{
    Player* player = botAI->GetBot();
    return botAI->CanCastSpell(spell, player) && botAI->CastSpell(spell, player);
}
}  // namespace

bool CanTaunt(PlayerbotAI* botAI)
{
    Player const* player = botAI->GetBot();
    TankSpells const* spells = FindTankSpells(player);
    return spells &&
           (spells->kind != StateKind::Form || IsPrepared(player, *spells, 0) || FindPreparation(player, *spells));
}

bool NeedsPreparation(PlayerbotAI* botAI)
{
    Player const* player = botAI->GetBot();
    TankSpells const* spells = FindTankSpells(player);
    uint32 const preparation = spells ? FindPreparation(player, *spells) : 0;
    return preparation && !IsPrepared(player, *spells, preparation);
}

bool Prepare(PlayerbotAI* botAI)
{
    Player* player = botAI->GetBot();
    TankSpells const* spells = FindTankSpells(player);
    uint32 const preparation = spells ? FindPreparation(player, *spells) : 0;
    if (!player->IsInWorld() || !player->IsAlive() || !preparation || IsPrepared(player, *spells, preparation))
        return false;

    TankState requested;
    requested.applied = preparation;
    requested.kind = spells->kind;
    TankState* saved = player->CustomData.Get<TankState>(STATE_KEY);
    requested.original = saved ? saved->original : GetCurrentStateSpell(player, requested);
    if (!saved && requested.original &&
        (!player->HasSpell(requested.original) || !player->HasAura(requested.original, player->GetGUID())))
        return false;

    if (requested.kind == StateKind::Form)
    {
        SpellInfo const* prepInfo = sSpellMgr->GetSpellInfo(preparation);
        if (!prepInfo)
            return false;

        if (prepInfo->CheckShapeshift(player->GetShapeshiftForm()) != SPELL_CAST_OK)
        {
            uint32 const active = GetActiveFormSpell(player);
            if (!active || !player->HasSpell(active) || !player->HasAura(active, player->GetGUID()))
                return false;

            // Some forms first require leaving the current form. Save it before removing its aura.
            player->RemoveAurasDueToSpell(active, player->GetGUID());
            *player->CustomData.GetDefault<TankState>(STATE_KEY) = requested;
            return true;
        }
    }

    if (!CastOnSelf(botAI, preparation))
        return false;

    *player->CustomData.GetDefault<TankState>(STATE_KEY) = requested;
    return true;
}

bool TryTaunt(PlayerbotAI* botAI, Unit* target)
{
    Player* player = botAI->GetBot();
    TankSpells const* spells = FindTankSpells(player);
    if (!spells || !target || !target->IsAlive() || !target->IsInWorld() || !player->IsInMap(target) ||
        !player->IsValidAttackTarget(target) || target->GetVictim() == player || !player->IsWithinLOSInMap(target) ||
        !IsPrepared(player, *spells, FindPreparation(player, *spells)))
        return false;

    return botAI->CanCastSpell(spells->taunt, target) && botAI->CastSpell(spells->taunt, target);
}

bool HasTemporaryState(PlayerbotAI* botAI) { return botAI->GetBot()->CustomData.Get<TankState>(STATE_KEY) != nullptr; }

bool Restore(PlayerbotAI* botAI)
{
    Player* player = botAI->GetBot();
    TankState* state = player->CustomData.Get<TankState>(STATE_KEY);
    if (!state || !player->IsInWorld() || !player->IsAlive())
        return false;

    uint32 const current = GetCurrentStateSpell(player, *state);
    bool const unrelatedForm = state->kind == StateKind::Form && !current && player->GetShapeshiftForm() != FORM_NONE;
    if (current == state->original || unrelatedForm || (current && current != state->applied))
    {
        player->CustomData.Erase(STATE_KEY);
        return false;
    }

    if (!state->original)
    {
        player->RemoveAurasDueToSpell(state->applied, player->GetGUID());
        player->CustomData.Erase(STATE_KEY);
        return true;
    }

    SpellInfo const* original = sSpellMgr->GetSpellInfo(state->original);
    if (!original || !player->HasSpell(state->original))
    {
        player->CustomData.Erase(STATE_KEY);
        return false;
    }

    if (state->kind == StateKind::Form && current == state->applied &&
        original->CheckShapeshift(player->GetShapeshiftForm()) != SPELL_CAST_OK)
    {
        player->RemoveAurasDueToSpell(current, player->GetGUID());
        return true;
    }

    if (!CastOnSelf(botAI, state->original))
        return false;

    player->CustomData.Erase(STATE_KEY);
    return true;
}

bool ConflictsWithTankControl(PlayerbotAI* botAI, Action* action, bool keepPreparation)
{
    auto* cast = dynamic_cast<CastSpellAction*>(action);
    if (!cast)
        return false;

    std::string const spell = cast->getSpell();
    constexpr std::array<std::string_view, 9> GENERIC_TAUNTS = {"taunt",
                                                                "growl",
                                                                "hand of reckoning",
                                                                "dark command",
                                                                "challenging shout",
                                                                "challenging roar",
                                                                "righteous defense",
                                                                "death grip",
                                                                "mocking blow"};
    for (std::string_view taunt : GENERIC_TAUNTS)
        if (spell == taunt)
            return true;

    if (!keepPreparation)
        return false;

    TankSpells const* spells = FindTankSpells(botAI->GetBot());
    if (!spells)
        return false;

    if (spells->kind == StateKind::Presence)
        return spell == "blood presence" || spell == "unholy presence";
    if (spells->kind != StateKind::Form)
        return false;

    if (spell == "caster form" || spell == "cancel bear form" || spell == "cancel dire bear form")
        return true;

    SpellInfo const* taunt = sSpellMgr->GetSpellInfo(spells->taunt);
    if (!taunt)
        return false;

    constexpr std::array<std::pair<std::string_view, ShapeshiftForm>, 12> FORMS = {{
        {"battle stance", FORM_BATTLESTANCE},
        {"defensive stance", FORM_DEFENSIVESTANCE},
        {"berserker stance", FORM_BERSERKERSTANCE},
        {"bear form", FORM_BEAR},
        {"dire bear form", FORM_DIREBEAR},
        {"cat form", FORM_CAT},
        {"tree of life", FORM_TREE},
        {"moonkin form", FORM_MOONKIN},
        {"travel form", FORM_TRAVEL},
        {"aquatic form", FORM_AQUA},
        {"flight form", FORM_FLIGHT},
        {"swift flight form", FORM_FLIGHT_EPIC},
    }};
    for (auto const& [name, form] : FORMS)
        if (spell == name)
            return taunt->CheckShapeshift(form) != SPELL_CAST_OK;
    return false;
}
}  // namespace EncounterTankActions
