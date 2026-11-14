/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 * Adapted from CodaBottle's Playerbots PR #2874 (1403cdb90a63a4e547d865cea59d1af335d2c93b).
 */

#ifndef PLAYERBOTS_AQ40STRATEGY_H
#define PLAYERBOTS_AQ40STRATEGY_H

#include "Strategy.h"

class Aq40ControlAction;

class RaidAq40Strategy : public Strategy
{
public:
    RaidAq40Strategy(PlayerbotAI* botAI) : Strategy(botAI) {}
    std::string const getName() override { return "aq40 cthun"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
    bool HasTargetExclusions() const override { return true; }
    void AppendTargetExclusions(GuidSet& exclusions, TargetValueExclusionType type) override;

private:
    // Target selection can ask for exclusions several times per tick; reuse them within the tick.
    GuidSet _exclusions;
    Aq40ControlAction const* _exclusionsControl = nullptr;
    uint32 _exclusionsVersion = 0;
    uint32 _exclusionsTime = 0;
};

#endif
