/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"

// --- The Underbog (map 546) — two-hop drop past Ghaz'an, ANCHORED ----------
//
// Like The Slave Pens, the route onward from the second boss crosses a BREAK in
// the navmesh: after Ghaz'an (second boss, bit 1) the party must descend toward
// Swamplord Musel'ek (bit 2) down a tiered slope that sits on disconnected mesh
// islands boss-nav cannot route across. Here the drop is TWO hops, not one — a
// single teleport would land the party too far ahead, so the descent is split
// with a pause in between to keep the bots from outrunning real players.
//
// A roster OBJECTIVE anchor (BossRosterRegistry map 546) sits at the upper ledge
// (274.72, -462.60, 81.37), ordered between Ghaz'an and Swamplord (encounterIndex
// 2, Swamplord's bit — the Objective-before-Boss tie-break in Apply() sorts it
// ahead of him); boss-nav drives the tank to it. This event then:
//   1. TELEPORTS the whole party down to the mid landing (333.63, -471.46, 52.10),
//   2. WAITS 10s so the party doesn't get too far ahead of the players,
//   3. TELEPORTS the party down to the lower landing (355.71, -471.68, 24.32),
// after which boss-nav resumes toward Swamplord from solid, connected mesh.
//
// PERSISTENT (unlike the single-hop Slave Pens teleport). The first teleport
// relocates the leader ~60yd from the anchor, so a non-persistent event's
// at-objective trigger would go false the next tick (tank no longer near the
// anchor) and the Wait + second teleport would never run — worse, the >1s-gap
// restart would rewind the chain to step 0. A Persistent event keeps its progress
// and its at-objective trigger stays sticky once started (IsPersistentAnchored-
// EventActive), so the tank can sit at the mid landing through the pause and the
// second hop fires. Each TeleportParty is itself idempotent (Done immediately if
// the leader is already on its landing), so a tick-gap restart never re-teleports.

void RegisterUnderbogEvents(std::vector<DungeonEvent>& out)
{
    out.push_back(EventBuilder(546, 1, "Drop down past Ghaz'an")
                      .Anchored(/*orderIndex, doc-only*/ 2)
                      .Persistent()
                      // Hop 1: upper ledge -> mid landing.
                      .TeleportParty(/*ledge*/ 274.72f, -462.60f, 81.37f,
                                     /*mid landing*/ 333.63f, -471.46f, 52.10f)
                      // Pause so the party doesn't outrun the human players.
                      .Wait(10000)
                      // Hop 2: mid landing -> lower landing (the checkpoint here is
                      // the mid landing the prior hop left the party on).
                      .TeleportParty(/*mid landing*/ 333.63f, -471.46f, 52.10f,
                                     /*lower landing*/ 355.71f, -471.68f, 24.32f)
                      .Build());
}

// --- roster patch (relocated from BossRosterRegistry) --------------------
void RegisterUnderbogRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    // --- The Underbog (map 546) ----------------------------------
    // The auto-roster derives all four bosses (Hungarfen 17770 / bit 0,
    // Ghaz'an 18105 / bit 1, Swamplord Musel'ek 17826 / bit 2, The Black
    // Stalker 17882 / bit 3) from their static spawns.
    //
    // GHAZ'AN needs his ANCHOR relocated. His derived anchor is his spawn
    // (193.68, -425.00, 43.54) — open water. Every navmesh column in that basin
    // is a WATER surface sheet at z ~= 50.8 over a floor at z ~= 3.6, so an
    // anchor there sends boss navigation SWIMMING: melee can't reach him,
    // healers lose LOS, and his Tail Sweep (34267) knockback drops whoever it
    // hits into a ~47yd pit. Re-anchor him on the dry upper-ring corridor at the
    // top of the south-east descent ramp — the same coordinates as his
    // BossPullbackRegistry row, which is what makes the party WALK there, hold,
    // and fight him on solid ground after the tank drags him up.
    //
    // remove + re-add (rather than `reorder`) because only remove+add can carry
    // hand-authored coordinates; completionFrom = his own entry keeps his real
    // DungeonEncounter kill-bit (1), resolved off the base list before the
    // removal takes effect (BossRosterRegistry::ApplyOne step 1).
    //
    // The path from Ghaz'an down to Swamplord additionally crosses a
    // navmesh BREAK — a tiered slope whose lower tiers sit on disconnected
    // mesh islands boss-nav can't route to.
    //
    // Add a travel OBJECTIVE at the upper ledge, ordered between Ghaz'an
    // and Swamplord. It borrows encounterIndex 2 (Swamplord's bit): the
    // Objective-before-Boss tie-break in Apply() sorts it AHEAD of
    // Swamplord at the shared key, so after Ghaz'an (bit 1) the tank
    // visits this objective and only then Swamplord. Sharing the bit is
    // safe — an objective is filtered by the cleared-anchor latch, never
    // the completion mask. Its eventId 1 (UnderbogEvents.cpp) does a TWO-
    // hop teleport (with a 5s pause between hops) down the break the
    // instant the tank reaches the ledge.
    {
        BossRosterPatch p;
        p.mapId = 546;
        p.remove = { 18105 };
        p.add = {
            // Ghaz'an, re-anchored onto dry ground (see the note above and
            // BossPullbackRegistry). Keeps his own kill-bit via completionFrom.
            MakeBoss(18105, 546, "Ghaz'an",
                     154.16f, -452.03f, 72.29f, /*completionFrom*/ 18105),
            MakeObjective(OBJ(1), /*encounterIndex*/ 2, 546,
                          "Drop down past Ghaz'an",
                          274.72f, -462.60f, 81.37f,
                          /*arriveRadius*/ 6.0f, /*gateEntry*/ 0,
                          /*hook*/ 0, /*eventId*/ 1),
        };
        t.push_back(std::move(p));
    }
}
