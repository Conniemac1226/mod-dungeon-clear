/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "Ai/Dungeon/DungeonClear/Data/FightInPlaceRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/ScriptedPullRegistry.h"

// The scripted-pull plan for Selin Fireheart's room (Magisters' Terrace, 585).
//
// Everything here is geometry the plan is only correct BECAUSE of — the pack
// cylinders sized to hold their own spawns and exclude their neighbours, the arm
// radius sized to sit between the antechamber and the doorway, and the stage
// ordering. A row edit that breaks any of it produces a run that looks healthy in
// the log and pulls the boss, which is exactly the failure mode seven previous
// attempts at this room died of. So it is pinned here rather than left to a live
// run to discover.

namespace
{
    uint32 constexpr MGT = 585;
    uint32 constexpr SKULKER = 24688;   // Wretched Skulker
    uint32 constexpr BRUISER = 24689;   // Wretched Bruiser
    uint32 constexpr HUSK    = 24690;   // Wretched Husk
    uint32 constexpr CRYSTAL = 24722;   // Fel Crystal — hostile prop, NOT a pack member
    uint32 constexpr SELIN   = 24723;

    ScriptedPullStage const& East()
    {
        ScriptedPullStage const* s = ScriptedPullRegistry::Find(MGT, 0);
        EXPECT_NE(s, nullptr);
        return *s;
    }
    ScriptedPullStage const& West()
    {
        ScriptedPullStage const* s = ScriptedPullRegistry::Find(MGT, 1);
        EXPECT_NE(s, nullptr);
        return *s;
    }
}

TEST(DcScriptedPullTest, MagistersTerraceHasTwoOrderedStages)
{
    EXPECT_TRUE(ScriptedPullRegistry::HasRows(MGT));
    EXPECT_FALSE(ScriptedPullRegistry::HasRows(0));
    EXPECT_FALSE(ScriptedPullRegistry::HasRows(560));   // Old Hillsbrad

    std::vector<ScriptedPullStage const*> const rows = ScriptedPullRegistry::Rows(MGT);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0]->order, 0u);
    EXPECT_EQ(rows[1]->order, 1u);
    // Both stages belong to Selin: the plan retires with him.
    EXPECT_EQ(rows[0]->bossEntry, SELIN);
    EXPECT_EQ(rows[1]->bossEntry, SELIN);
    // One camp for the whole plan — the party never relocates between pulls.
    EXPECT_FLOAT_EQ(rows[0]->campX, rows[1]->campX);
    EXPECT_FLOAT_EQ(rows[0]->campY, rows[1]->campY);
}

TEST(DcScriptedPullTest, FindRejectsUnknownMapsAndOrders)
{
    EXPECT_EQ(ScriptedPullRegistry::Find(MGT, -1), nullptr);   // the "no stage" sentinel
    EXPECT_EQ(ScriptedPullRegistry::Find(MGT, 2), nullptr);
    EXPECT_EQ(ScriptedPullRegistry::Find(0, 0), nullptr);
}

TEST(DcScriptedPullTest, PackCylindersHoldTheirOwnSpawns)
{
    // East pack: six Wretched at X 222-230, Y -16..-23. Every corner of that box
    // must read as inside its stage.
    EXPECT_TRUE(ScriptedPullRegistry::InPack(East(), 222.0f, -16.0f, -2.9f));
    EXPECT_TRUE(ScriptedPullRegistry::InPack(East(), 230.0f, -16.0f, -2.9f));
    EXPECT_TRUE(ScriptedPullRegistry::InPack(East(), 222.0f, -23.0f, -2.9f));
    EXPECT_TRUE(ScriptedPullRegistry::InPack(East(), 230.0f, -23.0f, -2.9f));

    // West pack: X 222-230, Y +17..+24.
    EXPECT_TRUE(ScriptedPullRegistry::InPack(West(), 222.0f, 17.0f, -2.9f));
    EXPECT_TRUE(ScriptedPullRegistry::InPack(West(), 230.0f, 17.0f, -2.9f));
    EXPECT_TRUE(ScriptedPullRegistry::InPack(West(), 222.0f, 24.0f, -2.9f));
    EXPECT_TRUE(ScriptedPullRegistry::InPack(West(), 230.0f, 24.0f, -2.9f));
}

TEST(DcScriptedPullTest, PackCylindersExcludeTheirNeighbours)
{
    // The centre pair (Skulker 231.3,2.8 and Bruiser 232.1,-2.0) belongs to the
    // BOSS pull, not to either stage — they sit ~10yd from Selin and cannot be
    // peeled off him. A cylinder that swallowed one would aim the stage at a mob
    // whose pull wakes the boss.
    EXPECT_FALSE(ScriptedPullRegistry::InPack(East(), 231.3f, 2.8f, -2.9f));
    EXPECT_FALSE(ScriptedPullRegistry::InPack(East(), 232.1f, -2.0f, -2.9f));
    EXPECT_FALSE(ScriptedPullRegistry::InPack(West(), 231.3f, 2.8f, -2.9f));
    EXPECT_FALSE(ScriptedPullRegistry::InPack(West(), 232.1f, -2.0f, -2.9f));

    // Selin himself (242.1, 0.3).
    EXPECT_FALSE(ScriptedPullRegistry::InPack(East(), 242.1f, 0.3f, -2.9f));
    EXPECT_FALSE(ScriptedPullRegistry::InPack(West(), 242.1f, 0.3f, -2.9f));

    // And the two stages never overlap each other.
    EXPECT_FALSE(ScriptedPullRegistry::InPack(East(), West().packX, West().packY, -2.9f));
    EXPECT_FALSE(ScriptedPullRegistry::InPack(West(), East().packX, East().packY, -2.9f));
}

TEST(DcScriptedPullTest, PackCylinderIsFloorBanded)
{
    EXPECT_TRUE(ScriptedPullRegistry::InPack(East(), East().packX, East().packY, -2.9f));
    // Far above/below the room's single floor: out, whatever the 2D distance says.
    EXPECT_FALSE(ScriptedPullRegistry::InPack(East(), East().packX, East().packY, 40.0f));
    EXPECT_FALSE(ScriptedPullRegistry::InPack(East(), East().packX, East().packY, -60.0f));
}

TEST(DcScriptedPullTest, OnlyTheGuardEntriesArePackMembers)
{
    for (ScriptedPullStage const* s : ScriptedPullRegistry::Rows(MGT))
    {
        EXPECT_TRUE(ScriptedPullRegistry::IsPackEntry(*s, SKULKER));
        EXPECT_TRUE(ScriptedPullRegistry::IsPackEntry(*s, BRUISER));
        EXPECT_TRUE(ScriptedPullRegistry::IsPackEntry(*s, HUSK));
        // The fel crystal sits at the dead centre of BOTH cylinders and is hostile
        // (faction 190). Counting it would mean the stage never reports its pack
        // cleared and the plan never advances to the next one.
        EXPECT_FALSE(ScriptedPullRegistry::IsPackEntry(*s, CRYSTAL));
        EXPECT_FALSE(ScriptedPullRegistry::IsPackEntry(*s, SELIN));
    }
}

TEST(DcScriptedPullTest, ArmRadiusSitsBetweenTheAntechamberAndTheDoor)
{
    // Armed once the tank is in the staging chamber (X 197-213) in front of the room.
    EXPECT_TRUE(ScriptedPullRegistry::InArmRange(East(), 197.0f, 5.0f, -2.9f));
    EXPECT_TRUE(ScriptedPullRegistry::InArmRange(East(), 209.0f, 5.6f, -2.9f));
    // Still armed at the doorway, so a stage that has to re-arm mid-room can.
    EXPECT_TRUE(ScriptedPullRegistry::InArmRange(East(), 216.0f, 0.0f, -2.9f));
    // NOT armed back at the last Sunblade pack before the room (all X <= 182.3): the
    // plan must not hijack the pull pipeline while that trash is the run's problem.
    EXPECT_FALSE(ScriptedPullRegistry::InArmRange(East(), 182.3f, 0.0f, -2.9f));
    EXPECT_FALSE(ScriptedPullRegistry::InArmRange(East(), 182.3f, 18.97f, -2.9f));
    // Or from the far side of the instance (Priestess Delrissa, X=126.9).
    EXPECT_FALSE(ScriptedPullRegistry::InArmRange(East(), 126.9f, 19.2f, -2.9f));
}

TEST(DcScriptedPullTest, TheArmGateIsAnchoredForwardOfTheCamp)
{
    // The arm gate answers "has the tank walked up to the ROOM", and for these rows
    // that is not "has it walked up to the CAMP" — the camp is mid-corridor, 43yd
    // back from the anchor and 11.5yd from the X 179-182 Sunblade pack.
    //
    // If the gate were measured from the camp, the stage would arm while that pack of
    // four elites is still alive: the pull pipeline would be taken off them, the tank
    // walked 40yd past them to a stand spot, and the followers pinned passive in the
    // middle of them. So every MgT row names an arm anchor, and it is far enough
    // forward of its camp that the arm circle cannot reach back to it.
    for (ScriptedPullStage const* s : ScriptedPullRegistry::Rows(MGT))
    {
        ASSERT_TRUE(s->HasArmAnchor()) << "stage " << s->order;
        float const dx = s->armX - s->campX;
        float const dy = s->armY - s->campY;
        EXPECT_GT(std::sqrt(dx * dx + dy * dy), s->armRadius)
            << "stage " << s->order
            << ": the arm radius reaches back to the camp, so the stage can arm "
               "before the trash standing at the camp is dead";
    }

    // Stated the other way round, in coordinates: standing ON the camp does not arm
    // the stage, and neither does standing on the pack that camps beside it
    // (X 179-182, |Y| <= 8 — see the row comments).
    EXPECT_FALSE(ScriptedPullRegistry::InArmRange(East(), East().campX, East().campY,
                                                 East().campZ));
    EXPECT_FALSE(ScriptedPullRegistry::InArmRange(East(), 179.02f, -7.98f, -2.63f));
    EXPECT_FALSE(ScriptedPullRegistry::InArmRange(East(), 182.34f, 4.85f, -2.66f));
}

TEST(DcScriptedPullTest, CampIsOutsideTheBossAggroGateAndStandsAreAuthored)
{
    // The camp must sit BELOW Selin's CanAIAttack plane (X > 216) — that plane is
    // why the party can hold there at all while the fight happens on top of them.
    EXPECT_LT(East().campX, 216.0f);
    EXPECT_LT(West().campX, 216.0f);
    // And outside the fight-in-place room, so the drag-back lands on ground the
    // rest of the pull pipeline already treats as safe.
    EXPECT_FALSE(FightInPlaceRegistry::IsNoPullZone(MGT, East().campX, East().campY));

    // BOTH stand spots are outside the room too. Each shoots a diagonal through
    // the doorway at the pack on the FAR side, so no part of the plan needs the
    // tank past Selin's plane until both packs are dead.
    EXPECT_LT(East().standX, 216.0f);
    EXPECT_LT(West().standX, 216.0f);
    // And they are a mirrored pair about the doorway's centre line.
    EXPECT_NEAR(East().standX, West().standX, 0.5f);
    EXPECT_NEAR(East().standY, -West().standY, 0.5f);
}

TEST(DcScriptedPullTest, TheCampIsFarEnoughBackToBeUnreachable)
{
    // The camp is not merely "outside the room": it is far enough back that the two
    // things that kept going wrong at a 20yd camp cannot physically happen. Both are
    // distances, so both are assertable.
    //
    // Two of the six mobs in each pack are Wretched Husks, which cast 44503 Fireball
    // and 44504 Frostbolt — both 40yd (Spell.dbc -> SpellRange.dbc) — off
    // smart_scripts rows carrying castFlags 64, SMARTCAST_COMBAT_MOVE. The core reads
    // that flag as "no combat movement while the target is in range AND in line of
    // sight", so inside 40yd a Husk's answer to being pulled is to step clear of the
    // wall and PLANT, which for this room means stopping in the doorway and holding
    // the fight open across it. Outside 40yd it has no such option and must run.
    //
    // Measured to the pack's OWN SPAWNS, not to the stand spot: the stand spot is a
    // proxy that happens to correlate, and this is the distance the mechanic uses.
    float constexpr kHuskSpellRange = 40.0f;
    // Selin's own leash. He only attacks targets at X > 216, so a boss accidentally
    // tagged and dragged toward this camp stops being able to attack anything and
    // resets. Worth having real margin: it turns the worst mistake in this room from
    // a wipe into a no-op.
    float constexpr kSelinPlaneX = 216.0f;

    // Real spawn positions (acore_world.creature, map 585) of both guard packs.
    std::vector<std::pair<float, float>> const guards{
        {224.41f, -16.27f}, {222.32f, -18.01f}, {222.65f, -20.81f},
        {228.56f, -16.65f}, {227.31f, -22.97f}, {230.53f, -20.94f},
        {225.52f,  16.98f}, {222.50f,  20.46f}, {228.52f,  17.92f},
        {230.19f,  19.71f}, {224.13f,  23.29f}, {228.04f,  23.75f}};

    for (ScriptedPullStage const* s : ScriptedPullRegistry::Rows(MGT))
    {
        float nearest = 1e9f;
        for (auto const& g : guards)
        {
            float const dx = g.first - s->campX;
            float const dy = g.second - s->campY;
            nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy));
        }
        EXPECT_GT(nearest, kHuskSpellRange)
            << "stage " << s->order << ": a Husk standing on its spawn is within "
            << nearest << "yd of the camp, so it can shoot the party from the room "
               "instead of being dragged out of it";

        EXPECT_GT(kSelinPlaneX - s->campX, kHuskSpellRange)
            << "stage " << s->order << ": the camp is not clear of Selin's plane by "
               "enough for a mis-tagged boss to reset on the way";

        // And the camp is BEHIND the last Sunblade pack before the room (X 179-182),
        // so the route has already cleared that pack by the time the party is asked
        // to hold there. A camp forward of it would pin the party into live trash.
        EXPECT_LT(s->campX, 179.0f) << "stage " << s->order;
    }
}

TEST(DcScriptedPullTest, TravelBudgetsCoverTheAuthoredDrag)
{
    // The forming dwell is measured across the camp-to-stand gap and sized from the
    // gap, not flat. The flat 8s it used to carry is ~5s of a 42yd walk at the rate
    // this budget assumes — so it expired before the party could park on a camp that
    // far back, and the tank tagged with the followers still strung out along the
    // corridor. Assert the budget the authored geometry actually needs.
    float const dx = East().campX - East().standX;
    float const dy = East().campY - East().standY;
    float const drag = std::sqrt(dx * dx + dy * dy);

    // Something crossing that gap at the pessimistic rate must fit inside the budget
    // with the base still to spare.
    EXPECT_GT(ScriptedPullTravelBudgetMs(drag),
              DC_SCRIPTED_PULL_TRAVEL_BASE_MS +
                  static_cast<uint32>(drag / 8.0f * 1000.0f))
        << "the forming dwell would expire while the party is still walking to camp";
    // And it is still BOUNDED — a follower that cannot path may not hold the run open.
    EXPECT_LT(ScriptedPullTravelBudgetMs(drag), 60000u);
    // Degenerate input is the base, never a division blow-up.
    EXPECT_EQ(ScriptedPullTravelBudgetMs(0.0f), DC_SCRIPTED_PULL_TRAVEL_BASE_MS);
    EXPECT_EQ(ScriptedPullTravelBudgetMs(-5.0f), DC_SCRIPTED_PULL_TRAVEL_BASE_MS);
}

TEST(DcScriptedPullTest, StandSpotsAreNotMobSpawnPoints)
{
    // A stand spot is measured in-game, and the first west row was — to two
    // decimals on all three axes — the spawn position of a Wretched Bruiser
    // (228.52, 17.92, -2.95). The plan walked the tank into the middle of the pack
    // for four test runs while the symptoms were patched downstream, because
    // nothing anywhere asserted that the spot the tank is sent to is EMPTY FLOOR.
    //
    // The live spawn boxes (acore_world.creature, map 585): the -Y pack occupies
    // X 222-231 / Y -16..-23 and the +Y pack X 222-231 / Y +17..+24. A stand spot
    // inside either box is a mob's feet, not a vantage point.
    //
    // Worth knowing when a coordinate is handed over for a row: `.gps` reports the
    // SELECTED unit's position, not the caller's, so a measurement taken with a mob
    // targeted is that mob's feet. One camp coordinate arrived that way and matched
    // Sunblade Magister 96787 to two decimals on all three axes. Cross-check a new
    // row against `creature` — an exact spawn match is the tell.
    auto inSpawnBox = [](float x, float y)
    {
        return x >= 221.0f && x <= 232.0f &&
               ((y >= 16.0f && y <= 25.0f) || (y >= -24.0f && y <= -15.0f));
    };

    for (ScriptedPullStage const* s : ScriptedPullRegistry::Rows(MGT))
    {
        EXPECT_FALSE(inSpawnBox(s->standX, s->standY))
            << "stage " << s->order << " stand spot (" << s->standX << ", "
            << s->standY << ") sits inside a guard pack's spawn box";
        EXPECT_FALSE(inSpawnBox(s->campX, s->campY))
            << "stage " << s->order << " camp sits inside a guard pack's spawn box";
        // A stand spot must also never be inside its OWN target volume — that is
        // the same error stated in the row's own terms.
        EXPECT_FALSE(ScriptedPullRegistry::InPack(*s, s->standX, s->standY, s->standZ));
    }
}

TEST(DcScriptedPullTest, EveryPackMemberIsInPullSpellRangeOfItsStandSpot)
{
    // The tag is taken FROM the stand spot, so the distance that matters is spot ->
    // pack member, and the plan is only viable while the NEAREST member is inside a
    // tank pull spell's reach. Avenger's Shield / Shield of the Templar reach 30yd,
    // and that has to be enough on its own: the clamp buys NOTHING extra now
    // (DC_SCRIPTED_PULL_CREEP is 0 — see TheTagIsTakenFromTheSpotAndNotAYardCloser).
    //
    // tr-20260802-215715-3 is why this is pinned: the scan handed the pull a member
    // 31.6yd from the east stand spot, the shield could not reach, and the generic
    // walk-in carried the tank off the spot and into the room. Ranking the pick
    // from the stand spot is the fix; this asserts the row geometry it relies on.
    float constexpr kPullSpellRange = 30.0f;

    auto nearestSpawnDist = [](ScriptedPullStage const& s,
                               std::vector<std::pair<float, float>> const& spawns)
    {
        float best = 1e9f;
        for (auto const& p : spawns)
        {
            float const dx = p.first - s.standX;
            float const dy = p.second - s.standY;
            best = std::min(best, std::sqrt(dx * dx + dy * dy));
        }
        return best;
    };

    // The REAL spawn positions (acore_world.creature, map 585), not a bounding box:
    // the west margin is thin (27.8yd of 30) and a box corner would flatter it.
    EXPECT_LT(nearestSpawnDist(East(), {{224.41f, -16.27f}, {222.32f, -18.01f},
                                        {222.65f, -20.81f}, {228.56f, -16.65f},
                                        {227.31f, -22.97f}, {230.53f, -20.94f}}),
              kPullSpellRange);
    EXPECT_LT(nearestSpawnDist(West(), {{225.52f, 16.98f}, {222.50f, 20.46f},
                                        {228.52f, 17.92f}, {230.19f, 19.71f},
                                        {224.13f, 23.29f}, {228.04f, 23.75f}}),
              kPullSpellRange);
}

TEST(DcScriptedPullTest, StandSpotsStayOutsideSelinsAggro)
{
    // Selin (24723) spawns at (242.07, 0.3) and reaches ~21yd against a level-70
    // party. Both stand spots have to clear that with real margin, because the tank
    // parks on one for the whole tag and the pack's run-in.
    float constexpr kSelinX = 242.07f, kSelinY = 0.3f, kSelinReach = 21.0f;
    for (ScriptedPullStage const* s : ScriptedPullRegistry::Rows(MGT))
    {
        float const dx = s->standX - kSelinX;
        float const dy = s->standY - kSelinY;
        EXPECT_GT(std::sqrt(dx * dx + dy * dy), kSelinReach)
            << "stage " << s->order << " stand spot is inside Selin's aggro";
    }
}

TEST(DcScriptedPullTest, CampLeashCannotReachTheRoom)
{
    // The leash bounds how far the tank may stray from camp during the camp fight
    // before it is walked back. It is only worth having if the furthest point it
    // tolerates is still short of the room — otherwise a chase excursion reaches
    // the doorway and the unpulled pack sees the tank anyway.
    //
    // The room's doorway sits at roughly (216, 0). With no arrival hold left, this
    // leash is the ONLY thing standing between a chase and the doorway for the whole
    // camp fight, so the bar it has to clear is the one that matters.
    float const dx = 216.0f - East().campX;
    float const dy = 0.0f - East().campY;
    float const campToDoor = std::sqrt(dx * dx + dy * dy);

    EXPECT_LT(DC_SCRIPTED_PULL_LEASH, campToDoor);
    // And the leash must sit outside the arrive radius the recall releases at (the
    // generic 5yd camp-arrive ball, file-local to DcPullActions), or the latch would
    // trip and clear on the same tick and produce the in-out shuffle.
    EXPECT_GT(DC_SCRIPTED_PULL_LEASH, 5.0f);
}

TEST(DcScriptedPullTest, TheTagIsTakenFromTheSpotAndNotAYardCloser)
{
    // The stand spots clear the CENTRE PAIR by about a yard, not by three, so any
    // creep toward the pack spends the whole margin. From the east stand spot
    // (212.22, 7.42) the two mobs flanking Selin sit at:
    //   Skulker 96825 (231.70,  2.63)  20.06yd
    //   Bruiser 96830 (231.62, -1.86)  21.50yd
    // and a level-69 elite reaches ~19yd against a level-70.
    //
    // tr-20260803-133734-1 is why this is pinned at zero: with 2.5yd of creep the tank
    // stood at ~(213.34, 5.19) — 18.54yd from that Skulker, inside aggro — and
    // body-pulled both centre mobs on the first pull. Creep also swings the sight-line
    // to them from Y~6.5 at the doorway (wall) to Y~4.8 (the opening), so it shortens
    // the range and grants line of sight at the same time.
    EXPECT_FLOAT_EQ(DC_SCRIPTED_PULL_CREEP, 0.0f);

    // The margin the spots actually have, asserted so a re-measure cannot quietly
    // erase it. Both stand spots must clear the centre pair's ~19yd reach.
    float constexpr kEliteReach = 19.0f;
    std::vector<std::pair<float, float>> const centrePair{{231.70f, 2.63f},
                                                          {231.62f, -1.86f}};
    for (ScriptedPullStage const* s : ScriptedPullRegistry::Rows(MGT))
    {
        for (auto const& c : centrePair)
        {
            float const dx = c.first - s->standX;
            float const dy = c.second - s->standY;
            EXPECT_GT(std::sqrt(dx * dx + dy * dy), kEliteReach)
                << "stage " << s->order << " stand spot is inside the centre pair's "
                   "aggro — the mobs flanking Selin, which no stage owns";
        }
    }
}

TEST(DcScriptedPullTest, TheRoomItselfIsTheKeepOutForFollowers)
{
    // The follower keep-out is a PLACE, not a radius: the fight-in-place row is the
    // room, and no party member may stand in it during a scripted stage. A radius
    // alone was not enough — a follower drifting inside its leash reads as "parked"
    // and yields the tick to whatever is carrying it, so it can cross the doorway
    // while still nominally in bounds.
    //
    // The camp and both stand spots must therefore be OUTSIDE the box, or the
    // keep-out would fire the moment anyone reached the spot they are told to
    // stand on.
    for (ScriptedPullStage const* s : ScriptedPullRegistry::Rows(MGT))
    {
        EXPECT_FALSE(FightInPlaceRegistry::IsNoPullZone(MGT, s->campX, s->campY));
        EXPECT_FALSE(FightInPlaceRegistry::IsNoPullZone(MGT, s->standX, s->standY));
    }
    // The doorway and the room floor beyond it ARE in the box — that is the point.
    EXPECT_TRUE(FightInPlaceRegistry::IsNoPullZone(MGT, 216.0f, 0.0f));
    EXPECT_TRUE(FightInPlaceRegistry::IsNoPullZone(MGT, 226.0f, 20.0f));
}

TEST(DcScriptedPullTest, TheFollowerLeashIsWiderThanTheGateThatWaitsOnIt)
{
    // The leash is a FIGHT radius and must not be the thing a passive follower parks
    // against. A radius is not a place: a bot stops the instant it crosses the
    // boundary, so parking against a leash settles it in a SHELL at leash distance on
    // whichever side it arrived from — the full 8yd off the authored point, in
    // different cover. Live (tr-20260803-121459-1): every passive tick logged
    // "parked" at 6.0-7.9yd and the party stood at (139.79, -7.66) while the row said
    // (134.14, -14.36), which reads as "the camp is nowhere near the coordinates".
    //
    // Two properties keep that from coming back, and the second is why the first is
    // not enough on its own:
    //   1. Passive followers pin to the slot, not the leash (DcFollowerActions).
    //   2. The leash still has to be WIDER than the gate that waits on the party, or
    //      a follower parked legitimately mid-fight would read as "not set" forever.
    EXPECT_GT(DC_SCRIPTED_PULL_FOLLOWER_LEASH, 2.0f)
        << "a fight radius this tight would fight the follower's own rotation";
    // And the slot pin a passive follower uses must be far tighter than the leash, or
    // switching between them achieves nothing.
    EXPECT_LT(2.0f, DC_SCRIPTED_PULL_FOLLOWER_LEASH * 0.5f);
}

TEST(DcScriptedPullTest, FollowerLeashIsTighterThanTheTanks)
{
    // The tank plants ON the camp and the pack piles onto it there, so a melee
    // follower needs its fuzzed slot offset plus melee reach — and no more.
    // Borrowing the tank's 12yd let a follower spend twelve yards of drift logging
    // "parked", which is to say YIELDING the tick to whatever was carrying it.
    EXPECT_LT(DC_SCRIPTED_PULL_FOLLOWER_LEASH, DC_SCRIPTED_PULL_LEASH);

    // But still wide enough to close on a mob standing on the tank, from the far
    // side of the camp slot fan: slot offset + melee reach + a little footwork.
    EXPECT_GT(DC_SCRIPTED_PULL_FOLLOWER_LEASH, 2.0f + 5.0f);
}

TEST(DcScriptedPullTest, LosingGroundIsARatchetNotATickDelta)
{
    // Three legs re-issue one unchanged destination every tick — the tank's drag-back,
    // the follower hold-at-camp, and the tank's camp-leash recall — and DcMoveTo
    // dedupes on destination, so the moment another generator takes the bot the leg
    // goes silent and the standing-still backstops stay blind (the bot is moving,
    // outward). Distance is the only evidence, and this is the read.
    //
    // tr-20260803-125341-1 is why the third leg exists: the recall had no ratchet, so
    // after the leash tripped at 13.2yd it issued nothing for twenty-one seconds while
    // MoveChase drove the tank to X~216 and into Selin's room.

    // No leg in flight -> never fires, whatever the distance.
    EXPECT_FALSE(ScriptedPullLostGround(0.0f, 500.0f));

    // Closing, or holding station, is not losing ground.
    EXPECT_FALSE(ScriptedPullLostGround(20.0f, 20.0f));
    EXPECT_FALSE(ScriptedPullLostGround(20.0f, 4.0f));

    // A RATCHET, not a tick-to-tick delta: path noise and the arc around a doorway
    // both give ground momentarily, so the tolerance has to absorb them.
    EXPECT_FALSE(ScriptedPullLostGround(20.0f, 20.0f + DC_SCRIPTED_PULL_LOSE_GROUND));
    EXPECT_TRUE(ScriptedPullLostGround(20.0f, 20.0f + DC_SCRIPTED_PULL_LOSE_GROUND + 0.1f));

    // The live shape: latched at 13.2yd, then carried outward by a chase.
    EXPECT_TRUE(ScriptedPullLostGround(13.2f, 17.0f));
    EXPECT_TRUE(ScriptedPullLostGround(13.2f, 46.0f));

    // And the tolerance must stay well inside the leash it defends, or the recall
    // would be re-issued only after the tank had already left the leash behind.
    EXPECT_LT(DC_SCRIPTED_PULL_LOSE_GROUND, DC_SCRIPTED_PULL_LEASH * 0.5f);
}

TEST(DcScriptedPullTest, ABodyPullFromTheStandSpotWakesTheCentrePair)
{
    // A tank with NO opener (a level-70 warrior whose ranged slot is empty or holds
    // the wrong ammo — see ResolveRangedWeaponPull) body-pulls instead of holding the
    // stand spot, because holding it means waiting out the whole leg budget for a tag
    // that can never fire and then walking at the boss anyway.
    //
    // This pins what that costs, so the trade-off is a recorded decision rather than
    // something rediscovered from a log. The walk-in runs from the stand spot to the
    // pack's nearest member, and on BOTH stages that line passes well inside the
    // ~19yd reach of the centre pair — the two mobs flanking Selin that no stage
    // owns. A body pull takes them too. Nothing tunable fixes it; the geometry is the
    // geometry, which is exactly why the ranged opener is worth keeping working.
    float constexpr kEliteReach = 19.0f;
    std::vector<std::pair<float, float>> const centrePair{{231.70f, 2.63f},
                                                          {231.62f, -1.86f}};
    // Nearest real spawn to each stand spot (creature rows on map 585).
    std::vector<std::pair<float, float>> const nearestMember{{224.41f, -16.27f},
                                                             {225.52f, 16.98f}};

    auto distToSegment = [](std::pair<float, float> const& p,
                            std::pair<float, float> const& a,
                            std::pair<float, float> const& b)
    {
        float const dx = b.first - a.first;
        float const dy = b.second - a.second;
        float const len = dx * dx + dy * dy;
        float t = len > 0.0f
            ? ((p.first - a.first) * dx + (p.second - a.second) * dy) / len
            : 0.0f;
        t = std::max(0.0f, std::min(1.0f, t));
        float const cx = a.first + t * dx;
        float const cy = a.second + t * dy;
        return std::sqrt((p.first - cx) * (p.first - cx) +
                         (p.second - cy) * (p.second - cy));
    };

    std::vector<ScriptedPullStage const*> const rows = ScriptedPullRegistry::Rows(MGT);
    ASSERT_EQ(rows.size(), nearestMember.size());
    for (size_t i = 0; i < rows.size(); ++i)
    {
        std::pair<float, float> const stand{rows[i]->standX, rows[i]->standY};
        for (auto const& c : centrePair)
        {
            EXPECT_LT(distToSegment(c, stand, nearestMember[i]), kEliteReach)
                << "stage " << rows[i]->order << ": the body-pull line now clears the "
                   "centre pair — if this is a real re-measure the fallback got safer, "
                   "but check it before relaxing anything that depends on it";
        }
    }

    // And the reason the trade is still worth taking: the drag-back delivers whatever
    // was woken to the row's CAMP, which is far outside the room rather than in the
    // doorway. A body pull is a worse pull than the authored one; it is a much better
    // one than standing still and then walking in with no camp at all.
    for (ScriptedPullStage const* s : rows)
    {
        float const dx = s->campX - s->standX;
        float const dy = s->campY - s->standY;
        EXPECT_GT(std::sqrt(dx * dx + dy * dy), 40.0f)
            << "stage " << s->order << ": camp is close enough to the stand spot that "
               "a body pull would fight next to the room it was dragged out of";
    }
}

TEST(DcScriptedPullTest, TheCampFightIsBoundedByProgressNotByAWallClock)
{
    // Engage was the one leg of a scripted pull with no watchdog at all, and it
    // retired on a single attacker-list read. tr-20260803-144046-4 is what that
    // costs when the read is wrong: "back on the camp (4.5yd) -> fighting" and then
    // four minutes and thirteen seconds of total silence — Engage for 254s, three
    // members combat-flagged, every victim empty and every health bar at 100%. A
    // latched stage stands the advance rung down and camp-holds the party, so no
    // other rung could break it.

    // Not sampled yet can never be stale, however long the run has been going.
    EXPECT_FALSE(ScriptedPullEngageStalled(0, 10u * 60u * 1000u));

    // The clock is re-armed by PROGRESS, so what it measures is time since the
    // pack's health last moved — not time in the phase. A fight that keeps landing
    // damage restamps `since` and can never trip, whatever its total length.
    uint32 constexpr kStart = 100000;
    EXPECT_FALSE(ScriptedPullEngageStalled(kStart, kStart));
    EXPECT_FALSE(ScriptedPullEngageStalled(kStart,
                                           kStart + DC_SCRIPTED_PULL_ENGAGE_STALL_MS));
    EXPECT_TRUE(ScriptedPullEngageStalled(
        kStart, kStart + DC_SCRIPTED_PULL_ENGAGE_STALL_MS + 1));

    // getMSTime() can read backwards across the sample and the compare (the same
    // millisecond-boundary race that made the Returning leg fire at random until it
    // was clamped). Never trip on an underflow.
    EXPECT_FALSE(ScriptedPullEngageStalled(kStart, kStart - 1));

    // Sized between the two things it has to tell apart: long enough that the
    // slowest healthy stage on record (tr-20260803-144046-8, 137s arm to done)
    // cannot trip it on a lull, short enough that the 254s freeze is caught with
    // most of it left.
    EXPECT_GT(DC_SCRIPTED_PULL_ENGAGE_STALL_MS, 30u * 1000u);
    EXPECT_LT(DC_SCRIPTED_PULL_ENGAGE_STALL_MS, 137u * 1000u);
}

TEST(DcScriptedPullTest, SelectOrderRunsTheLowestLiveStage)
{
    std::vector<uint32> const orders{0u, 1u};

    // Both packs up -> east first. That ordering is the plan: the east pack is the
    // one the tank has a safe sight-line to while the party is still walking up.
    EXPECT_EQ(ScriptedPullRegistry::SelectOrder(orders, {true, true}, -1), 0);
    // East cleared -> west.
    EXPECT_EQ(ScriptedPullRegistry::SelectOrder(orders, {false, true}, -1), 1);
    // Both cleared -> nothing due; the run walks in and takes the boss.
    EXPECT_EQ(ScriptedPullRegistry::SelectOrder(orders, {false, false}, -1), -1);
    // A stage that fizzled and left survivors behind re-arms ahead of the next one.
    EXPECT_EQ(ScriptedPullRegistry::SelectOrder(orders, {true, false}, -1), 0);
}

TEST(DcScriptedPullTest, SelectOrderKeepsACommittedStageWhileItsPackIsDraggedOut)
{
    std::vector<uint32> const orders{0u, 1u};

    // THE case this pin exists for: the tank has tagged the east pack and is
    // hauling it back to camp, so the east VOLUME reads empty mid-drag. Without the
    // pin the plan would hand the tank the west pack while it is still running home
    // with the east one.
    EXPECT_EQ(ScriptedPullRegistry::SelectOrder(orders, {false, true}, 0), 0);
    // The pin holds even when nothing anywhere reads live.
    EXPECT_EQ(ScriptedPullRegistry::SelectOrder(orders, {false, false}, 0), 0);
    // A pin naming a stage this map does not have is ignored, not trusted.
    EXPECT_EQ(ScriptedPullRegistry::SelectOrder(orders, {false, true}, 7), 1);
}

TEST(DcScriptedPullTest, SelectOrderHandlesEmptyInput)
{
    // No stage passed the boss/arm gates this tick.
    EXPECT_EQ(ScriptedPullRegistry::SelectOrder({}, {}, -1), -1);
    EXPECT_EQ(ScriptedPullRegistry::SelectOrder({}, {}, 0), -1);
    // Only the west stage is in arm range: its own order is what comes back, not
    // its index in the (filtered) list.
    EXPECT_EQ(ScriptedPullRegistry::SelectOrder({1u}, {true}, -1), 1);
}
