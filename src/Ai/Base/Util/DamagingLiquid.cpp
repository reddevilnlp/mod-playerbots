/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DamagingLiquid.h"

#include "DBCStores.h"
#include "GridTerrainData.h"
#include "Map.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"

namespace DamagingLiquid
{
    uint32 HarmfulSpell(uint32 liquidEntry)
    {
        LiquidTypeEntry const* liquid = sLiquidTypeStore.LookupEntry(liquidEntry);
        if (!liquid || !liquid->SpellId)
            return 0;

        SpellInfo const* spell = sSpellMgr->GetSpellInfo(liquid->SpellId);
        return spell && !spell->IsPositive() ? liquid->SpellId : 0;
    }

    bool IsIn(Unit const* unit)
    {
        if (!unit)
            return false;

        LiquidData const& liquid = unit->GetLiquidData();
        return (liquid.Status & MAP_LIQUID_STATUS_SWIMMING) && HarmfulSpell(liquid.Entry);
    }

    bool IsAt(Unit const* unit, float x, float y, float z)
    {
        if (!unit || !unit->GetMap())
            return false;

        LiquidData const liquid =
            unit->GetMap()->GetLiquidData(unit->GetPhaseMask(), x, y, z, unit->GetCollisionHeight(), {});
        return (liquid.Status & MAP_LIQUID_STATUS_SWIMMING) && HarmfulSpell(liquid.Entry);
    }
}
