/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 * Adapted from CodaBottle's Playerbots PR #2874 (1403cdb90a63a4e547d865cea59d1af335d2c93b).
 */

#ifndef PLAYERBOTS_AQ40ACTIONCONTEXT_H
#define PLAYERBOTS_AQ40ACTIONCONTEXT_H

#include "Aq40Actions.h"
#include "NamedObjectContext.h"

class RaidAq40ActionContext : public NamedObjectContext<Action>
{
public:
    RaidAq40ActionContext()
    {
        creators["aq40 control"] = &Control;
        creators["aq40 safety"] = &Safety;
        creators["aq40 tactics"] = &Tactics;
        creators["aq40 position"] = &Positioning;
        creators["aq40 cthun interrupt"] = &CthunInterrupt;
        creators["aq40 cthun prepull"] = &CthunPrepull;
    }

private:
    static Action* Control(PlayerbotAI* botAI) { return new Aq40ControlAction(botAI); }
    static Action* Safety(PlayerbotAI* botAI) { return new Aq40MoveAction(botAI, true); }
    static Action* Tactics(PlayerbotAI* botAI) { return new Aq40MoveAction(botAI, false, true); }
    static Action* Positioning(PlayerbotAI* botAI) { return new Aq40MoveAction(botAI, false); }
    static Action* CthunInterrupt(PlayerbotAI* botAI) { return new Aq40CthunInterruptAction(botAI); }
    static Action* CthunPrepull(PlayerbotAI* botAI) { return new Aq40CthunPrepullAction(botAI); }
};

#endif
