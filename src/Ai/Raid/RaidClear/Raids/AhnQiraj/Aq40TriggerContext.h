/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 * Adapted from CodaBottle's Playerbots PR #2874 (1403cdb90a63a4e547d865cea59d1af335d2c93b).
 */

#ifndef PLAYERBOTS_AQ40TRIGGERCONTEXT_H
#define PLAYERBOTS_AQ40TRIGGERCONTEXT_H

#include "Aq40Actions.h"
#include "NamedObjectContext.h"

class RaidAq40TriggerContext : public NamedObjectContext<Trigger>
{
public:
    RaidAq40TriggerContext()
    {
        creators["aq40 encounter"] = &Encounter;
        creators["aq40 cthun prepull"] = &CthunPrepull;
    }

private:
    static Trigger* Encounter(PlayerbotAI* botAI) { return new Aq40Trigger(botAI); }
    static Trigger* CthunPrepull(PlayerbotAI* botAI) { return new Aq40CthunPrepullTrigger(botAI); }
};

#endif
