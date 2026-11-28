/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_DAMAGINGLIQUID_H
#define PLAYERBOTS_DAMAGINGLIQUID_H

#include "Define.h"

class Unit;

// Liquids whose LiquidType.dbc row applies a harmful spell to anything swimming in them
// (Naxxramas slime: 28801, -90% stats and 500 damage every 2s). The core casts that spell
// from Unit::ProcessTerrainStatusUpdate while the unit's liquid status is "swimming", which
// includes standing in a shallow pool, so "swimming" is the test here too.
namespace DamagingLiquid
{
    // The harmful spell a liquid type applies, or 0.
    uint32 HarmfulSpell(uint32 liquidEntry);

    // O(1): reads the unit's cached liquid status.
    bool IsIn(Unit const* unit);

    // Map liquid query at a point, sized to the unit's collision height.
    bool IsAt(Unit const* unit, float x, float y, float z);
}

#endif
