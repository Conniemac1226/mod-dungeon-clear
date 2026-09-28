/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonRosterBuilders.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "InstanceScript.h"

// --- Karazhan (map 532) ----------------------------------------------------
//
// Plan: deployment-files/docs/mod-dungeon-clear_karazhan_plan.md.
//
// mod-playerbots ships a `karazhan` raid strategy that owns the fights (Attumen's
// phases, Moroes' adds, the Opera plays, Aran, Netherspite's beams, Nightbane's
// flight). This file owns only what happens OUT of combat: the roster, and the
// events that start the Opera and summon Nightbane.
//
// The auto-derived roster has ten anchors, built from instance_encounters credit
// spawns. The patch corrects four things:
//
//   * ATTUMEN IS MISSING. His credit entry is the mounted Attumen (16152), which
//     is summoned mid-fight and has no spawn row. The encounter is engaged
//     through Midnight (16151): neutral and REACT_DEFENSIVE, so nothing pulls him
//     but an attack. EngageDirect force-engages a non-hostile boss, so no special
//     case is needed. Midnight never dies on his own — he sits at a 1-HP floor,
//     goes invisible when the two merge, and is killed by the mounted Attumen's
//     JustDied, which is also what sets bit 0. MakeBossWithBit is the escape hatch
//     for exactly this: a real DBC bit whose derivation failed.
//
//     Midnight's evade boundary is a 50yd circle around his spawn (z 49-55), and
//     he despawns 10s after an evade. Boss anchors are engaged where the boss
//     stands (only BossPullbackRegistry drags one, and it has no row here), so
//     the fight stays in the stables.
//
//   * CHESS is skipped by design (it will be its own plan), and PRINCE with it:
//     only Chess's DONE handler opens the Gamesman's Hall Exit Door (184277),
//     the only way to Prince. Drop the Prince skip when Chess ships.
//
//   * OPERA's credit spawn is Barnes (16812), a friendly NPC whose gossip starts
//     the play. He is removed here; the Opera event re-adds the encounter as an
//     objective.
//
//   * NIGHTBANE's spawn is a perch at (-11003.7, -1760.2, 140.3), on a mesh
//     island with no connection to the inside, and NON_ATTACKABLE. He is removed
//     and re-added on the Master's Terrace behind the Blackened Urn event.
//
// Order: Attumen, Moroes, Maiden, Opera, Nightbane, Curator, Terestian, Aran,
// Netherspite. Nightbane's slot was measured on the navmesh: fitting the urn in
// after Opera costs 249yd of extra walking, the next best slot 693yd.

using namespace DcKarazhan;

namespace
{
    // The Opera. Barnes (friendly) offers his gossip only once Moroes is DONE and
    // the Opera is not: menu 7421 option 0, then the C++ submenu 7422 option 0,
    // which calls StartEvent — the slot goes IN_PROGRESS and he walks his escort
    // path (script_waypoint 16812): waypoint 0 opens Stage Door Left, waypoint 4
    // is ~45s of speech on the stage, waypoint 8 shuts Stage Door Left again
    // (not for Hood), and waypoint 9 opens the curtain and summons the cast,
    // NON_ATTACKABLE, on the line y -1758.
    //
    // Which play runs is rolled per instance, so the cast tells us: Dorothee for
    // Oz, Grandmother for Red Riding Hood, Julianne for Romulo and Julianne. Oz
    // and R&J release their cast into zone combat on timers. Hood waits for a
    // player to talk to Grandmother (7441 -> 7442 -> 7443, option 0 each); the
    // last select summons the Big Bad Wolf, which attacks the one who talked —
    // the tank, as it should be.
    //
    // A cast member evading sets FAIL, reopens Stage Door Left and respawns
    // Barnes for another go. DONE opens both stage doors. So the event owns the
    // whole encounter, retry included, and its objective completes on the slot
    // reading DONE (doneBossStateIndex):
    //
    //   0. raid muster at Barnes: everyone topped off and on the tank. Nobody
    //      may stop to drink backstage once he walks, or waypoint 8 locks them
    //      out of the fight;
    //   1. talk to Barnes;
    //   2. walk onto the stage with him and hold until the cast appears;
    //   3. Hood only: talk to Grandmother (skipped when she is not there);
    //   4. hold on the stage until DONE. The plays are combat, which the
    //      playerbots `karazhan` strategy owns (Oz priority, the Wolf kite, the
    //      R&J skull marks). FAIL rewinds to step 0.
    DungeonEvent Opera()
    {
        return EventBuilder(MAP, EV_OPERA, "Opera: Barnes")
            .Anchored(ORDER_OPERA)
            .Persistent()  // the plays are several combat gaps; never rewind on one
            .Custom(DC_HOOK_RAID_MUSTER)
                .Timeout(240000)
            .Gossip(NPC_BARNES, /*option*/ 0, /*searchRadius*/ 30.0f)
                .WaitTargetStill()
            .MoveToHoldUntilSpawn(STAGE_X, STAGE_Y, STAGE_Z, /*radius*/ 4.0f, NPC_DOROTHEE)
                .OrSpawnOf({ NPC_GRANDMOTHER, NPC_JULIANNE })
                .Timeout(180000)  // the walk and the speech run ~60-65s
            .Gossip(NPC_GRANDMOTHER, /*option*/ 0, /*searchRadius*/ 40.0f)
                .SkipIfTargetMissing()
            .MoveToHoldUntilBossState(STAGE_X, STAGE_Y, STAGE_Z, /*radius*/ 20.0f,
                                      static_cast<uint32>(STATE_OPERA), DcBossStateBit(DONE))
                .RestartOnBossState(DcBossStateBit(FAIL) | DcBossStateBit(NOT_STARTED))
                .Timeout(1800000)
            .Build();
    }

    // Nightbane. The Blackened Urn (lock 1691, LOCKTYPE_OPEN: no key, no skill)
    // runs go_blackened_urn, which starts the intro only while his slot is
    // NOT_STARTED and he is alive. mod-individual-progression rebinds that script
    // name to one that also demands item 24140 in the clicker's bags and refuses
    // silently otherwise, so the click step carries it (CarryItem). He takes off from the perch, flies path 172250
    // and lands at LANDING; 3s later he turns attackable and calls
    // SetInCombatWithZone, which puts the slot IN_PROGRESS and shuts both terrace
    // doors (DOOR_TYPE_ROOM). 8s after landing he evades unless a player is within
    // 45yd. An evade resets the slot to NOT_STARTED, flies him home, and despawns
    // and respawns him there, which re-arms the urn. He cannot die before his
    // third air phase.
    //
    // So the event owns the whole encounter, retry included, and its objective
    // completes on the slot reading DONE (doneBossStateIndex), not on the click.
    // The objective is anchored on the urn itself (URN_ARRIVE_RADIUS), so the
    // clear only starts the event once the tank is in click range:
    //
    //   0. click the urn;
    //   1. raid muster: stage on the tank, top off, rebuff, while he flies the
    //      intro path (~765yd, about a minute). The terrace doors shut when the
    //      fight starts, so everyone must be out here first;
    //   2. hold at the muster point (18.8yd from the landing) until the fight is
    //      on — this is what stops the 8s no-player evade;
    //   3. hold until DONE. The fight is combat, which the playerbots `karazhan`
    //      strategy owns (flight phases, teleport back to the terrace). An evade
    //      reads NOT_STARTED here and rewinds to step 0, which clicks again.
    DungeonEvent NightbaneUrn()
    {
        return EventBuilder(MAP, EV_NIGHTBANE, "Nightbane: Blackened Urn")
            .Anchored(ORDER_NIGHTBANE_URN)
            .Persistent()  // the fight is several combat gaps; never rewind on one
            .UseGO(GO_BLACKENED_URN, /*searchRadius*/ 30.0f, URN_X, URN_Y, URN_Z)
                .CarryItem(ITEM_BLACKENED_URN)
                .Timeout(60000)
            .Custom(DC_HOOK_RAID_MUSTER)
                .Timeout(240000)
            .MoveToHoldUntilBossState(NB_MUSTER_X, NB_MUSTER_Y, NB_MUSTER_Z, /*radius*/ 8.0f,
                                      static_cast<uint32>(STATE_NIGHTBANE),
                                      DcBossStateBit(IN_PROGRESS) | DcBossStateBit(DONE))
                .Timeout(150000)  // take-off 4s + the flight path + 3s on the ground
            .MoveToHoldUntilBossState(NB_MUSTER_X, NB_MUSTER_Y, NB_MUSTER_Z, /*radius*/ 25.0f,
                                      static_cast<uint32>(STATE_NIGHTBANE), DcBossStateBit(DONE))
                .RestartOnBossState(DcBossStateBit(NOT_STARTED) | DcBossStateBit(FAIL))
                .Timeout(1800000)
            .Build();
    }
}

void RegisterKarazhanEvents(std::vector<DungeonEvent>& out)
{
    out.push_back(Opera());
    out.push_back(NightbaneUrn());
}

void RegisterKarazhanRoster(std::vector<BossRosterPatch>& t)
{
    using namespace DcRoster;

    BossRosterPatch p;
    p.mapId = MAP;

    p.add.push_back(MakeBossWithBit(NPC_MIDNIGHT, MAP, "Attumen the Huntsman",
                                    MIDNIGHT_X, MIDNIGHT_Y, MIDNIGHT_Z, BIT_ATTUMEN,
                                    ORDER_ATTUMEN));

    p.remove = { NPC_BARNES, NPC_NIGHTBANE };

    // Opera: the Barnes objective owns the encounter (see Opera) and is finished
    // when the Opera slot reads DONE. Barnes' own row is removed above: a boss
    // anchor on a friendly NPC would have the tank attack him.
    DungeonBossInfo opera = MakeObjective(OBJ(2), /*encounterIndex*/ BIT_OPERA, MAP,
                                          "Opera: Barnes", BARNES_X, BARNES_Y, BARNES_Z,
                                          /*arriveRadius*/ 8.0f, /*gateEntry*/ 0, /*hook*/ 0,
                                          EV_OPERA, ORDER_OPERA);
    opera.doneBossStateIndex = STATE_OPERA;
    p.add.push_back(opera);

    // Nightbane: the urn objective owns the encounter (see NightbaneUrn) and is
    // finished when his slot reads DONE. The boss row after it is re-added on the
    // terrace landing with his real bit 10, for completion and the panel; by the
    // time the clear reaches it the bit is already set.
    DungeonBossInfo urn = MakeObjective(OBJ(1), /*encounterIndex*/ BIT_NIGHTBANE, MAP,
                                        "Nightbane: Blackened Urn", URN_X, URN_Y, URN_Z,
                                        URN_ARRIVE_RADIUS, /*gateEntry*/ 0, /*hook*/ 0,
                                        EV_NIGHTBANE, ORDER_NIGHTBANE_URN);
    urn.doneBossStateIndex = STATE_NIGHTBANE;
    p.add.push_back(urn);
    p.add.push_back(MakeBoss(NPC_NIGHTBANE, MAP, "Nightbane",
                             LANDING_X, LANDING_Y, LANDING_Z,
                             /*completionFrom*/ NPC_NIGHTBANE, ORDER_NIGHTBANE));
    p.skipByDesign = { NPC_CHESS, NPC_PRINCE };

    // The DBC order is Attumen, Moroes, Maiden, Opera, Curator, Terestian, Aran,
    // Netherspite, Chess, Prince, Nightbane. Curator onward move up two slots to
    // make room for the urn objective and Nightbane after the Opera.
    p.reorder = {
        { NPC_MOROES,      ORDER_MOROES },
        { NPC_MAIDEN,      ORDER_MAIDEN },
        { NPC_CURATOR,     ORDER_CURATOR },
        { NPC_TERESTIAN,   ORDER_TERESTIAN },
        { NPC_ARAN,        ORDER_ARAN },
        { NPC_NETHERSPITE, ORDER_NETHERSPITE },
        { NPC_CHESS,       ORDER_CHESS },
        { NPC_PRINCE,      ORDER_PRINCE },
    };

    t.push_back(std::move(p));
}

// The entrance to Attumen, the stables. Midnight's pen is a walled circle that
// opens only to the WEST; the corridor to it runs up the east side, along the
// pen's east wall (y -1948, 18-19yd from Midnight in a straight line, 65-90yd
// on foot), round the south arc and in from the west. The at-boss handoff reads
// straight-line distance on the boss's own floor, so without this row it fires
// on the east wall and the engage opens Midnight through it with the stalls
// still up (tr-20260923-183454-1). Anchored segments hold the handoff until the
// tank stands on the last one, so the party clears the ring on the way round
// and engages from the west.
//
// Decimated from the Detour corridor TestKarazhanRouteProbe prints; re-derive it
// there after an mmaps regen rather than editing by hand.
void RegisterKarazhanRoute()
{
    DungeonClearRouteRegistry::Register(
        MAP, DUNGEON_DIFFICULTY_NORMAL, NPC_MIDNIGHT,
        {
            { -11100.00f, -2003.98f, 50.03f },  // 0  the entrance
            { -11094.60f, -1980.60f, 50.35f },  // 1  through the Gatehouse
            { -11089.20f, -1957.21f, 50.53f },  // 2
            { -11096.63f, -1948.19f, 50.53f },  // 3  the turn into the stables
            { -11112.54f, -1947.98f, 50.53f },  // 4
            { -11124.53f, -1948.53f, 50.53f },  // 5  the pen's east wall
            { -11135.56f, -1943.86f, 50.53f },  // 6
            { -11149.28f, -1929.31f, 50.72f },  // 7  the south arc
            { -11138.80f, -1917.55f, 50.29f },  // 8
            { WEST_STANDOFF_X, WEST_STANDOFF_Y, WEST_STANDOFF_Z },  // 9  the pen's west mouth
        });

    // Attumen to Moroes, the way players go: back out round the stables to the
    // Gatehouse by the entrance, then up the east stairs into the Banquet Hall.
    // Detour's shortest walk (293.5yd against this row's ~370) instead climbs
    // the stair at the stables' north end into the Servants' Quarters and comes
    // in on Moroes from the west, which is where the tank went without it.
    // Starts on Midnight's spawn, where the party stands when Attumen dies.
    DungeonClearRouteRegistry::Register(
        MAP, DUNGEON_DIFFICULTY_NORMAL, NPC_MOROES,
        {
            { MIDNIGHT_X, MIDNIGHT_Y, 49.80f },                     // 0  Midnight's pen
            { WEST_STANDOFF_X, WEST_STANDOFF_Y, WEST_STANDOFF_Z },  // 1  the pen's west mouth
            { -11143.37f, -1921.29f, 50.42f },  // 2
            { -11146.26f, -1924.05f, 50.53f },  // 3
            { -11148.10f, -1931.75f, 50.53f },  // 4  the south arc
            { -11135.56f, -1941.68f, 50.60f },  // 5
            { -11134.95f, -1945.64f, 50.53f },  // 6
            { -11131.09f, -1946.71f, 51.02f },  // 7
            { -11123.39f, -1948.85f, 50.55f },  // 8  the pen's east wall
            { -11107.39f, -1948.32f, 50.53f },  // 9
            { -11095.40f, -1947.93f, 50.53f },  // 10 out of the stables
            { -11089.55f, -1958.09f, 50.53f },  // 11
            { -11093.57f, -1977.63f, 50.35f },  // 12 through the Gatehouse
            { -11097.36f, -1993.17f, 50.53f },  // 13 back at the entrance
            { -11094.13f, -1994.34f, 50.51f },  // 14
            { -11076.86f, -1984.26f, 50.50f },  // 15
            { -11066.18f, -1978.80f, 54.40f },  // 16
            { -11055.00f, -1967.77f, 54.27f },  // 17
            { -11039.00f, -1967.45f, 54.93f },  // 18 foot of the east stairs
            { -11023.11f, -1965.54f, 65.62f },  // 19
            { -11015.17f, -1964.58f, 68.31f },  // 20
            { -11011.20f, -1964.11f, 68.29f },  // 21 landing
            { -11007.55f, -1962.46f, 69.48f },  // 22
            { -10991.56f, -1961.86f, 80.20f },  // 23 top of the stairs
            { -10980.97f, -1956.40f, 81.14f },  // 24
            { -10973.28f, -1955.13f, 80.80f },  // 25
            { -10971.15f, -1943.33f, 79.60f },  // 26 the Banquet Hall
            { -10974.15f, -1927.61f, 80.72f },  // 27
            { -10974.89f, -1923.68f, 80.82f },  // 28
            { -10975.64f, -1919.75f, 79.61f },  // 29
            { -10979.38f, -1900.11f, 79.76f },  // 30
            { -10980.13f, -1896.18f, 79.63f },  // 31
            { -10980.88f, -1892.25f, 82.35f },  // 32 Moroes' dais
        });
}

