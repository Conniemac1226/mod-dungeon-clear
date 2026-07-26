/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <cmath>

#include "Ai/Dungeon/DungeonClear/Data/BossPullbackRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"

// Pull-back bosses (BossPullbackRegistry) — the "this boss must be fought
// somewhere else" table, and the roster patch that has to agree with it.
//
// The table is tiny and hand-authored, so what is worth pinning is not the lookup
// (trivial) but the INVARIANTS that make the feature safe. Every one of these
// caught a real class of authoring mistake during development:
//   * a row whose anchor drifts from the roster anchor (the party walks to one
//     place and the maneuver drags the boss to another),
//   * the row leaking onto other maps or other bosses.

namespace
{
    constexpr uint32 kUnderbog = 546;
    constexpr uint32 kGhazan = 18105;

    // The measured navmesh facts this row is built on (see BossPullbackRegistry.cpp
    // for how they were obtained). Repeated here so a future edit to the row has to
    // consciously break a documented number rather than silently drift.
    constexpr float kLakeSurfaceZ = 50.8f;    // water sheet over Ghaz'an's basin
    constexpr float kGhazanHomeX = 193.74f;   // waypoint 1383920 point 20 = his home
    constexpr float kGhazanHomeY = -423.40f;
}

TEST(DungeonClearBossPullbackTest, GhazanHasARow)
{
    BossPullback const* row = BossPullbackRegistry::Find(kUnderbog, kGhazan);
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->mapId, kUnderbog);
    EXPECT_EQ(row->bossEntry, kGhazan);
}

TEST(DungeonClearBossPullbackTest, HasRowsGatesByMap)
{
    EXPECT_TRUE(BossPullbackRegistry::HasRows(kUnderbog));
    // The cheap early-out every other map relies on. If this ever goes true for a
    // map with no row, every tick on that map starts paying for the cross-context
    // pull-back probes.
    EXPECT_FALSE(BossPullbackRegistry::HasRows(0));
    EXPECT_FALSE(BossPullbackRegistry::HasRows(545));   // The Slave Pens
    EXPECT_FALSE(BossPullbackRegistry::HasRows(585));   // Magisters' Terrace
}

TEST(DungeonClearBossPullbackTest, OnlyGhazanIsPulledBackInTheUnderbog)
{
    // The Underbog's other three bosses are fought where they stand; a row on any
    // of them would silently reroute their whole engagement.
    EXPECT_EQ(BossPullbackRegistry::Find(kUnderbog, 17770), nullptr);  // Hungarfen
    EXPECT_EQ(BossPullbackRegistry::Find(kUnderbog, 17826), nullptr);  // Swamplord Musel'ek
    EXPECT_EQ(BossPullbackRegistry::Find(kUnderbog, 17882), nullptr);  // The Black Stalker
}

TEST(DungeonClearBossPullbackTest, RowDoesNotLeakToOtherMaps)
{
    // Same entry, different map: no row. (Guards against a Find that matches on
    // entry alone — which would make every map with a same-entry creature behave
    // as if it had a pull-back anchor at Underbog coordinates.)
    EXPECT_EQ(BossPullbackRegistry::Find(545, kGhazan), nullptr);
    EXPECT_EQ(BossPullbackRegistry::Find(0, kGhazan), nullptr);
}

TEST(DungeonClearBossPullbackTest, GhazanAnchorIsOutOfTheLake)
{
    BossPullback const* row = BossPullbackRegistry::Find(kUnderbog, kGhazan);
    ASSERT_NE(row, nullptr);

    // The anchor is the whole point: it must be well ABOVE the lake surface, not
    // merely away from the boss. An anchor at or below the water sheet would be a
    // spot the party can be knocked off of into deep water.
    EXPECT_GT(row->campZ, kLakeSurfaceZ + 15.0f);

    // ...and genuinely far from where the boss lives, so the maneuver is a real
    // drag rather than a fight on his doorstep. (Straight-line; the actual route
    // is ~2.7x this.)
    float const dx = row->campX - kGhazanHomeX;
    float const dy = row->campY - kGhazanHomeY;
    EXPECT_GT(std::sqrt(dx * dx + dy * dy), 40.0f);
}

// Force-aggro is a PER-ENCOUNTER opt-in, not a property of being a pull-back
// boss. Forcing bypasses the boss's own aggro logic — which is normal, tuned
// behaviour everywhere else — so the default has to be "off", and a new row must
// have to type the range out deliberately rather than inherit it.
TEST(DungeonClearBossPullbackTest, ForceAggroDefaultsOff)
{
    // THE guard for future rows: a row that says nothing about force-aggro gets
    // none. If this default ever becomes nonzero, every row added afterwards
    // silently starts force-pulling its boss.
    BossPullback const fresh;
    EXPECT_FLOAT_EQ(fresh.forceAggroRange, 0.0f);
}

// Same contract for the summon. Relocating a boss outright is a bigger hammer
// than forcing his aggro, so it has to be at least as hard to acquire by
// accident: off unless a row asks for it by name.
TEST(DungeonClearBossPullbackTest, SummonWhenStuckDefaultsOff)
{
    BossPullback const fresh;
    EXPECT_FALSE(fresh.summonWhenStuckBelow);
}

TEST(DungeonClearBossPullbackTest, GhazanOptsIntoTheStuckSummon)
{
    BossPullback const* row = BossPullbackRegistry::Find(kUnderbog, kGhazan);
    ASSERT_NE(row, nullptr);

    // He needs it because his way out of the lake — the pipe — is one of the
    // pieces the mmap extractor dropped, so a chase path off the water has no
    // geometry to follow and he can hang at the edge forever. Waiting longer
    // cannot help, which is what makes relocating him the right call HERE and
    // nowhere else so far.
    EXPECT_TRUE(row->summonWhenStuckBelow);

    // The summon only ever fires while he is BELOW the anchor, so an anchor that
    // stopped being above the water would silently disable it. Pinned alongside
    // the anchor's own height assertion so the two cannot drift apart.
    EXPECT_GT(row->campZ, kLakeSurfaceZ);
}

TEST(DungeonClearBossPullbackTest, GhazanOptsIntoForceAggroAcrossHisWholeLap)
{
    BossPullback const* row = BossPullbackRegistry::Find(kUnderbog, kGhazan);
    ASSERT_NE(row, nullptr);

    // He is the one encounter that opts in: there is no walkable spot inside his
    // aggro bubble (platform and pipe are off-mesh), so a natural tag is not
    // merely awkward, it is impossible.
    EXPECT_GT(row->forceAggroRange, 0.0f);

    // The range has to cover his ENTIRE waypoint lap, not just his platform —
    // the failure being handled is him STALLING partway round, still in the water.
    // Path 1383920's far point is (278.4, -477.4); measure the anchor to it.
    float const dxLap = row->campX - 278.4f;
    float const dyLap = row->campY - (-477.4f);
    float const lapReach = std::sqrt(dxLap * dxLap + dyLap * dyLap);
    EXPECT_GT(row->forceAggroRange, lapReach)
        << "force range must reach the far end of Ghaz'an's lap (" << lapReach
        << "yd), else the tank walks out to a stalled boss — i.e. swims";

    // ...but still bounded. An unbounded range would force-pull him from the far
    // side of the dungeon before the party has even arrived.
    EXPECT_LT(row->forceAggroRange, 250.0f);
}

TEST(DungeonClearBossPullbackTest, RosterAnchorMatchesThePullbackAnchor)
{
    // THE invariant. Boss navigation walks the party to the ROSTER anchor; the
    // maneuver drags the boss to the REGISTRY camp. If the two ever drift apart
    // the party holds in one place and the boss is delivered to another — the
    // party would stand at the anchor watching the tank solo him elsewhere.
    BossPullback const* row = BossPullbackRegistry::Find(kUnderbog, kGhazan);
    ASSERT_NE(row, nullptr);

    bool found = false;
    for (BossRosterPatch const& patch : BossRosterRegistry::AllPatches())
    {
        if (patch.mapId != kUnderbog)
            continue;
        for (DungeonBossInfo const& b : patch.add)
        {
            if (b.entry != kGhazan)
                continue;
            found = true;
            EXPECT_EQ(b.kind, DungeonAnchorKind::Boss);
            EXPECT_FLOAT_EQ(b.x, row->campX);
            EXPECT_FLOAT_EQ(b.y, row->campY);
            EXPECT_FLOAT_EQ(b.z, row->campZ);
            // Re-added rather than reordered, so it must inherit its own kill-bit
            // back off the base list — otherwise the boss would carry encounter
            // index 0 and be confused with Hungarfen's completion.
            EXPECT_EQ(b.inheritCompletionFrom, kGhazan);
        }
        // ...and the derived (in-water) anchor must actually be removed, else both
        // copies survive and the clear visits the lake one anyway.
        bool removed = false;
        for (uint32 e : patch.remove)
            if (e == kGhazan)
                removed = true;
        EXPECT_TRUE(removed);
    }
    EXPECT_TRUE(found) << "The Underbog roster patch no longer re-anchors Ghaz'an";
}
