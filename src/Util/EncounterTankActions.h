/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ENCOUNTERTANKACTIONS_H
#define PLAYERBOTS_ENCOUNTERTANKACTIONS_H

class Action;
class PlayerbotAI;
class Unit;

namespace EncounterTankActions
{
// Preparation and restoration return true when they cast a spell or remove an aura.
bool CanTaunt(PlayerbotAI* botAI);
bool NeedsPreparation(PlayerbotAI* botAI);
bool Prepare(PlayerbotAI* botAI);
bool TryTaunt(PlayerbotAI* botAI, Unit* target);
bool HasTemporaryState(PlayerbotAI* botAI);
bool Restore(PlayerbotAI* botAI);
bool ConflictsWithTankControl(PlayerbotAI* botAI, Action* action, bool keepPreparation);
}  // namespace EncounterTankActions

#endif
