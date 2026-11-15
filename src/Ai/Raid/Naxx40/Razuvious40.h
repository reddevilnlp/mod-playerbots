/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_RAZUVIOUS40_H
#define PLAYERBOTS_RAZUVIOUS40_H

#include <vector>

class Action;
class Creature;
class Multiplier;
class PlayerbotAI;
class Trigger;
class TriggerNode;

// Direct an existing Mind Control, including before the boss has entered combat.
bool DirectRazuvious40Understudy(PlayerbotAI& ai, Creature& boss);

Action* CreateRazuvious40PositionAction(PlayerbotAI* ai);
Trigger* CreateRazuvious40PositionTrigger(PlayerbotAI* ai);
void AddRazuvious40Triggers(std::vector<TriggerNode*>& triggers);
void AddRazuvious40Multipliers(PlayerbotAI* ai, std::vector<Multiplier*>& multipliers);

#endif
