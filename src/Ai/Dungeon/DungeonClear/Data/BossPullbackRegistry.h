/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_BOSSPULLBACKREGISTRY_H
#define _PLAYERBOT_BOSSPULLBACKREGISTRY_H

#include "Define.h"

// Static registry of PULL-BACK bosses: bosses that must never be fought where
// they stand, because the ground they stand on kills the party.
//
// The canonical case is Ghaz'an (18105, The Underbog 546). He is a hydra whose
// home is OPEN WATER: the areatrigger at (234.98, -379.28, 72.52) sends him down
// waypoint path 1383920 to (193.74, -423.40, 43.58), where boss_ghazan stamps
// that as his home position and does MoveRandom(12). The navmesh there is a water
// SURFACE sheet at z ~= 50.8 over a floor at z ~= 3.6 — a ~47yd deep pit. Bots
// routed at his live position snap to the surface sheet and SWIM: melee can't
// reach him, healers lose line of sight, and his Tail Sweep (34267) knockback
// puts whoever it hits into open water. That is the reported wipe.
//
// The fix is positional and hand-authored, in the same spirit as
// FightInPlaceRegistry (which forbids a pull) — this is its mirror image: it
// MANDATES one. A row says "the party fights this boss HERE, not where he lives".
// The tank walks the party to `camp`, goes out ALONE to tag the boss, drags him
// back, and the whole party fights on the anchor.
//
// Everything after that is the EXISTING advanced-pull machinery, unchanged: the
// Forming/Advancing/Returning/Engage FSM, the follower hold-at-camp, and the
// drag-back action all run exactly as they do for a trash pull. A row only
// changes WHICH target the pull is aimed at and WHERE the camp is.
//
// Why a registry and not geometry: "is this spot lethal" is not derivable. The
// navmesh happily reports the water sheet as walkable (it IS — you can swim it),
// the mob is reachable, and there is no aura or hazard emitter to detect. Only a
// human who has watched the encounter knows the party has to stand somewhere
// else. Mirrors RoomAggroRegistry / BossRosterRegistry: adding a fix is a single
// table edit inside DungeonClear/, never a core change or an mmap regen.
struct BossPullback
{
    uint32 mapId{0};
    uint32 bossEntry{0};
    // Party fight anchor: hand-authored, on safe ground, verified against the
    // navmesh. This is ALSO the boss's roster anchor (see the matching
    // BossRosterPatch), so boss navigation walks the party here instead of
    // routing them at the boss's live position.
    float  campX{0.0f}, campY{0.0f}, campZ{0.0f};

    // FORCE-AGGRO opt-in. 0 (the default) means "tag this boss the normal way" —
    // the tag leg walks inside his aggro bubble and lets him notice the tank, the
    // same pull every other boss in every other dungeon gets. A positive value is
    // the range, in yards, within which the tank instead forces him into combat
    // outright (DcForcePullbackAggro).
    //
    // DEFAULT OFF ON PURPOSE, and it should stay the exception. Forcing bypasses
    // the boss's own aggro logic, which is normal, tuned behaviour we want almost
    // everywhere: it can start an encounter from outside the range the script
    // expects, skip a script's own aggro hooks, and it is indiscriminate about
    // where the boss is standing when it lands. Setting this is a statement that a
    // SPECIFIC boss cannot be tagged normally at all — not a shortcut for one that
    // is merely awkward.
    //
    // Ghaz'an is the case it exists for: his platform and the pipe to it are
    // missing from the extracted navmesh and he does not reliably finish his
    // waypoint lap, so there is no reachable spot inside his aggro bubble and the
    // tag leg can only ever time out. See the row's own note.
    float  forceAggroRange{0.0f};

    // SUMMON-IF-STUCK opt-in, and like forceAggroRange it defaults OFF and should
    // stay rare — this one more so, because it relocates a boss outright.
    //
    // It fires in exactly one situation: the tank is home at the anchor, the boss
    // is engaged and coming, and he is STILL IN THE WATER BELOW the anchor. Then he
    // is teleported to the anchor rather than waited on. It is not a shortcut for a
    // slow boss — the water-and-below test is what keeps it to the failure it is
    // for, and a boss that has climbed out onto dry ground is left alone to walk
    // the rest of the way himself.
    //
    // Ghaz'an needs it because his route out of the lake is not something the
    // server can reliably walk him along: the pipe he is scripted to climb is
    // absent from the extracted navmesh, so his chase path off the water has no
    // geometry to follow and he can hang at the water's edge indefinitely. Waiting
    // longer does not help — nothing is going to change — so the choice is between
    // relocating him and stalling the run.
    bool   summonWhenStuckBelow{false};
};

class BossPullbackRegistry
{
public:
    // The pull-back row for (mapId, bossEntry), or nullptr when the boss is
    // fought normally. Pure (no game state) so it is unit-testable on its own.
    // Linear scan; the table is tiny.
    static BossPullback const* Find(uint32 mapId, uint32 bossEntry);

    // True iff `mapId` has any pull-back boss. Cheap early-out for the per-tick
    // callers (the engage gate and the camp guard) so a map with no rows pays one
    // bool and nothing else.
    static bool HasRows(uint32 mapId);
};

#endif  // _PLAYERBOT_BOSSPULLBACKREGISTRY_H
