/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "BossPullbackRegistry.h"

namespace
{
    // ---- the table ------------------------------------------------------
    //
    // The Underbog (546) — Ghaz'an (18105). Measured off the extracted navmesh
    // and acore_world, not guessed:
    //
    //   * Ghaz'an's post-areatrigger home is (193.74, -423.40, 43.58) (waypoint
    //     path 1383920, point 20; boss_ghazan::MovementInform stamps it as home
    //     then MoveRandom(12)). Every navmesh column across that whole basin is
    //     WATER at z ~= 50.8 over GROUND at z ~= 3.6 — he is swimming, ~7yd under
    //     the surface, over a ~47yd pit. There is no walkable ledge anywhere
    //     inside his wander radius.
    //   * The party's clear route reaches the basin down the south-east ramp:
    //     ... (184.6,-486.4,63.7) -> (197.7,-484.0,58.8) -> (208.1,-479.6,54.1),
    //     which is the LAST dry ground, then water at (210.1,-473.9,51.6).
    //   * The anchor below sits at the TOP of that ramp on the upper ring, on a
    //     ~24yd-wide dry corridor (navmesh ground z 72.5-73.6 across x 152..176,
    //     y -420..-472) with no water within 30yd in any direction. It is where
    //     the party finishes the last trash pack (Murkblood/Wrathfin at
    //     158-168, -425..-440) before descending.
    //
    // Coordinates are the ones measured in-game standing on the spot.
    //
    // NB the drag is LONG — ~147yd of path from the anchor to Ghaz'an, of which
    // ~65yd is open water and ~60yd is the ramp. That is deliberate: it is the
    // nearest ground the party can fight on without a knockback reaching water.
    // The drag legs get their own watchdog for it (see DcPullActions).
    //
    // FORCE-AGGRO (the trailing 150) is enabled for Ghaz'an and SHOULD NOT be
    // copied onto a new row by default — see the field's note in the header. He
    // needs it because he cannot be tagged normally at all:
    //   * His platform and the pipe leading to it were dropped by the mmap
    //     extractor, so there is no walkable spot inside his aggro bubble. The tag
    //     leg's whole approach — creep to the aggro edge, hold, let him notice us —
    //     has nowhere to stand.
    //   * He does not reliably finish the lap either. When he stalls partway he is
    //     still out in the water, further out still, and the tag leg could only
    //     burn its watchdog holding for an aggro that never comes.
    // A boss that is merely awkward to reach does NOT qualify; leave this 0 and let
    // the normal tag run.
    //
    // The range covers his ENTIRE lap rather than just his platform: path 1383920
    // swings out to (278.4, -477.4), ~130yd from the anchor, so 150 lets the tank
    // pull from the anchor wherever along it he stalled and never leave safe
    // ground. He does the travelling.
    // SUMMON-IF-STUCK (the trailing true) is likewise Ghaz'an-only. Once aggroed he
    // has to climb out of the lake to reach the party, and the pipe he climbs is
    // one of the pieces the mmap extractor dropped — so his chase path off the
    // water has no geometry to follow and he can hang at the water's edge for good.
    // Waiting longer changes nothing, so when the tank is home and he is still in
    // the water below the anchor he is relocated to it. Leave this false on any row
    // whose boss can actually walk to the anchor.
    BossPullback const kRows[] =
    {
        // map    entry   anchor x         y          z      forceAggro  summonIfStuck
        {  546,   18105,  154.16f,  -452.03f,   72.29f,  150.0f,      true  },  // The Underbog — Ghaz'an
    };
}

BossPullback const* BossPullbackRegistry::Find(uint32 mapId, uint32 bossEntry)
{
    for (BossPullback const& r : kRows)
        if (r.mapId == mapId && r.bossEntry == bossEntry)
            return &r;
    return nullptr;
}

bool BossPullbackRegistry::HasRows(uint32 mapId)
{
    for (BossPullback const& r : kRows)
        if (r.mapId == mapId)
            return true;
    return false;
}
