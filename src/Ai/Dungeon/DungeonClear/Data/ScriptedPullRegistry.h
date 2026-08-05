/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_SCRIPTEDPULLREGISTRY_H
#define _PLAYERBOT_SCRIPTEDPULLREGISTRY_H

#include <cstdint>
#include <vector>

#include "Define.h"

class Player;
class Unit;
class AiObjectContext;

// Static registry of SCRIPTED PULL STAGES: hand-authored "peel this pack out of
// that room, in this order, from this exact spot" plans.
//
// It is the third member of the positional-override family and sits between the
// other two. FightInPlaceRegistry FORBIDS a pull; BossPullbackRegistry MANDATES
// one for a boss and hand-authors its camp. This one mandates a SEQUENCE of trash
// pulls and hand-authors both the camp AND the spot the tank takes the tag from.
//
// Why the extra spot. The generic pull picks its own commit point — it glides
// toward the pack and stops just outside the pack's aggro bubble. That works in a
// corridor, where "just outside aggro" and "out of everyone else's aggro" are the
// same place. It does not work in a room whose packs share sight-lines with each
// other and with a boss: there the only safe place to stand is a specific spot
// behind a specific piece of wall, which has line of sight to exactly one pack and
// to nothing else. That spot is not derivable from the navmesh or from aggro
// radii — it is a fact about the room's walls that a human measured in-game.
//
// The canonical case is Selin Fireheart's room (Magisters' Terrace, 585). Selin
// only ever attacks targets inside his room (CanAIAttack is
// `who->GetPositionX() > 216.0f`), his two six-mob guard packs sit either side of
// him well inside that plane, and every route that walks the party in far enough
// to fight a pack also wakes him. Seven sessions of trying to fight the packs in
// place, in a corner, ended with the boss pulled every time. The design this
// registry serves inverts it: the party camps WELL BACK DOWN THE HALL, and the
// tank steps to one measured spot per pack — both of them outside the room,
// shooting a diagonal through the doorway at the pack on the far side — tags at
// range, and drags it all the way back out. The party never enters the room until
// both packs are dead. See the MgT rows in ScriptedPullRegistry.cpp for the
// geometry.
//
// WELL BACK is doing work in that sentence, and the distance that matters is not to
// the doorway — it is 40 YARDS TO THE NEAREST GUARD SPAWN. Two of the six mobs in
// each pack are Wretched Husks, which cast 44503 Fireball / 44504 Frostbolt (both
// 40yd) off smart_scripts rows carrying castFlags 64, SMARTCAST_COMBAT_MOVE. The
// core reads that flag as "no combat movement while the target is in range AND in
// line of sight". So inside 40yd a Husk's answer to being pulled is to step clear of
// whatever was blocking line of sight and then PLANT and shoot — for this room, to
// stop in the doorway and hold the fight open across it. Outside 40yd it has no such
// option and must run the corridor, which is what a drag-back is for.
//
// The camp began ~20yd from the doorway, well inside that, and four rounds of fixes
// went into policing the consequences. It is now mid-corridor at 55yd from the
// nearest guard spawn, which buys three things at once, none of them a gate that can
// fail:
//   * THE CASTERS HAVE TO COME. See above; this is the whole reason for the number.
//   * SELIN CANNOT FOLLOW. An accidental boss tag used to mean a boss fight at the
//     camp; from 45yd under his CanAIAttack plane, a dragged Selin crosses it long
//     before he arrives, stops being able to attack anything, and resets. The worst
//     mistake available in this room becomes free.
//   * THE LEASHES STOP BEING LOAD-BEARING. A 12yd tank excursion covered ~60% of the
//     distance to the doorway before; now it covers 26%.
// What it costs is drag length — ~42yd instead of ~15 — so anything clocked against
// the short version has to be sized from the row instead of flat (see
// ScriptedPullTravelBudgetMs below and its two use sites), and the arm gate can no
// longer be measured from the camp at all (see `armX`).
//
// A stand spot is measured in-game and cannot be sanity-checked by eye: the first
// west row here was, to two decimals on all three axes, the SPAWN POSITION OF ONE
// OF THE MOBS, and the plan dutifully walked the tank into the middle of the pack
// for four test runs while every gate downstream was patched around the symptom.
// Cross-check a new row against `creature` before trusting it — the row-geometry
// gtests in t/TestScriptedPull.cpp exist for exactly this.
//
// Everything after the plan is the EXISTING advanced-pull machinery, unchanged:
// the Forming/Advancing/Returning/Engage FSM, the follower hold-at-camp, the
// drag-back action. A stage only changes WHICH pack the pull is aimed at, WHERE
// the party camps, and WHERE the tank stands to tag.
struct ScriptedPullStage
{
    uint32 mapId{0};
    // The stage only arms while this boss is the run's next objective, so the
    // plan can never fire on the way past the room after the boss is dead.
    uint32 bossEntry{0};
    // Pull sequence within the map, ascending. Stage N+1 only arms once stage N's
    // volume holds no live pack member — i.e. once that pack has been peeled out
    // and killed. Must be unique per map (it is the stage's identity: the in-flight
    // stage is remembered as DcPullContext::scriptedStage).
    uint32 order{0};
    char const* name{nullptr};

    // Party camp: where the followers hold, passive, and where the tank drags the
    // pack back to. Hand-authored OUT of the pack's line of sight.
    float campX{0.0f}, campY{0.0f}, campZ{0.0f};

    // Tank stand spot: the one place with line of sight to THIS pack and to
    // nothing else. The tag leg walks here first and pulls from here.
    float standX{0.0f}, standY{0.0f}, standZ{0.0f};

    // The pack, as a cylinder: everything in `entries` within `packRadius` (2D)
    // and `packZBand` (vertical half-band) of (packX,packY,packZ). Sized to hold
    // the pack's spawns and to EXCLUDE its neighbours — a stage that swallowed the
    // boss or the next pack would select the wrong target and never complete.
    float packX{0.0f}, packY{0.0f}, packZ{0.0f};
    float packRadius{0.0f};
    float packZBand{0.0f};

    // Arm gate: the stage is dormant until the tank is within `armRadius` (2D) of
    // the arm anchor. Without it the plan would arm from the instance entrance —
    // the boss is "next" from the first tick — and hijack the pull pipeline while
    // the trash between here and there is still up. Size the radius so it covers
    // the staging ground in front of the room and stops short of the last pack
    // before it.
    //
    // (0,0,0) => measure from the CAMP, which is the right anchor whenever the camp
    // is a few yards behind the pack, i.e. for every row where "the tank has walked
    // up to the camp" and "the tank has walked up to the room" are the same event.
    //
    // They are NOT the same event once the camp is backed off far enough to be out of
    // the pack's reach, and conflating them is a live hazard, not a tidiness point.
    // Selin's camp is mid-corridor, 43yd back from the anchor and 11.5yd from the
    // X 179-182 Sunblade pack; an arm radius of 25 measured from THERE arms the stage
    // while that pack is still alive. The plan would hijack the pull pipeline off it,
    // walk the tank 40yd past it to the stand spot, and pin the followers, passive, in
    // the middle of it. So a row whose camp is out of reach of its own work names its
    // arm anchor separately, and Selin's stays where it has always been: the staging
    // chamber in front of the doorway.
    float armX{0.0f}, armY{0.0f}, armZ{0.0f};
    float armRadius{0.0f};

    // True once a row names an arm anchor distinct from its camp.
    bool HasArmAnchor() const
    {
        return armX != 0.0f || armY != 0.0f || armZ != 0.0f;
    }

    // Only these creature entries count as pack members. Position alone is not
    // enough: rooms contain props that read as hostile (Selin's fel crystals are
    // faction 190 and sit at the centre of both guard packs), and a stage that
    // counted one would never report its pack cleared.
    std::vector<uint32> entries;
};

// Vertical tolerance, in yards, for the arm-range test (a bot on the room's floor
// vs an anchor coordinate measured a fraction of a yard off it). Deliberately
// loose: the arm gate is about "have we walked up to the room yet", not precision.
inline constexpr float DC_SCRIPTED_PULL_ARM_ZBAND = 20.0f;

// How far past the stand spot the tag walk-in may travel, in yards.
//
// ZERO. The tag is taken FROM the spot, full stop. This began as 2.5yd of slack for
// "a mob a hair outside the pull spell's range", which sounded harmless and is not:
// the stand spots have a yard of aggro margin, not three, and 2.5yd of creep spends
// all of it.
//
// Measured, from the EAST stand spot (212.22, 7.42) — the numbers the original row
// comment worked through for the west spot and never did for this one:
//   centre-pair Skulker 96825 (231.70,  2.63)   20.06yd
//   centre-pair Bruiser 96830 (231.62, -1.86)   21.50yd
// A level-69 elite reaches ~19yd against a level-70, so the spot itself clears them
// by about a yard. Creep 2.5yd toward the east pack and the tank stands at
// (213.34, 5.19): 18.54yd from that Skulker — inside aggro — and the sight-line to it
// now leaves the doorway at Y~4.8 instead of Y~6.5, i.e. through the opening instead
// of into the wall. The creep shortens the range AND opens line of sight, and
// tr-20260803-133734-1 is what that looks like: the tank body-pulled both centre mobs
// on the first pull, which is a fight the plan never accounted for right next to the
// boss.
//
// Nothing is lost by removing it. Both packs' nearest member is inside a 30yd opener
// from its own spot (east ~25yd, west ~27.8yd), so the slack was never needed here,
// and a genuinely unreachable tag still falls out to the leg watchdog and the normal
// walk-in exactly as before — bounded, and loud in the log.
inline constexpr float DC_SCRIPTED_PULL_CREEP = 0.0f;

// --- holding the fight at the camp ---------------------------------------
// A drag-back ends the moment the TANK is home, on a scripted stage exactly as on an
// ordinary trash pull: arriving IS the end of the maneuver, and the flip to Engage
// releases tank and party together.
//
// It briefly did not. Because the tag is taken at range from a stand spot, the pack
// starts its run ~42yd out and the tank is home several seconds ahead of it, and a
// GATHER radius held the tank on the camp until every live attacker had run in — on
// the theory that stock combat, handed a victim that far away, would chase it back
// into the room. But the drag is what empties the room: by the time the tank stands
// on the camp the whole pack is loose in the corridor behind it, so the chase that
// gate feared is a chase down open hallway, not into the room. Meanwhile the hold
// cost a real thing — the tank standing still, back to an inbound six-mob pack,
// building no threat while the released DPS opened on the runners.
//
// So ONE gate scoped to a scripted stage remains:
//
//   LEASH   for the whole camp fight, how far the tank may stray from camp before it
//           is walked back. Generous on purpose: it exists to catch a chase
//           excursion, not to fight the tank's own footwork. It used to have to stay
//           inside the ~20yd from camp to the doorway; with the camp 45yd back that
//           ceiling is slack and 12yd is simply "planted, with footwork". This — not
//           an arrival hold — is what keeps the tank off the doorway, and it applies
//           for the entire fight rather than just its first seconds.
inline constexpr float  DC_SCRIPTED_PULL_LEASH   = 12.0f;

// --- clocks on a scripted stage's ground --------------------------------------
// A scripted stage's distances are authored, not emergent, and a row is free to put
// its camp a long way from its stand spot. Every wait measured across that gap has
// to be sized from the gap itself; the flat numbers below are only the floor.
//
// The floor, in ms: what something crossing a camp-to-stand gap of a few yards
// needs. This is the whole budget these waits used to have, back when that
// described every row.
inline constexpr uint32 DC_SCRIPTED_PULL_TRAVEL_BASE_MS = 8000;

// Yards per second credited to whatever is crossing that ground. Wretched trash and
// a bot both run at ~7-8 yd/s; 5 is deliberately pessimistic, because these budgets
// are WATCHDOGS — they must not expire on a healthy crossing, and paying for that
// with a slower assumed speed costs nothing, since each wait ends the moment the
// thing actually arrives rather than at its clock.
inline constexpr float  DC_SCRIPTED_PULL_TRAVEL_YD_PER_SEC = 5.0f;

// How long to allow for something to cross `yards` of a scripted stage's ground.
//
// THE FORMING DWELL uses it: the tank waiting for the party to park at camp before it
// tags. A flat 8s broke that once Selin's camp moved 42yd from its stand spots —
// roughly 6-9s of travel, which 8s only just fails to cover and then fails EVERY
// pull, so the dwell expired and the tank tagged with the followers still strung out
// along the hall behind it. (The gather hold was the other caller, until arrival
// stopped being something the tank waits past — see DC_SCRIPTED_PULL_LEASH above.)
//
// Still BOUNDED, for the original reason: a follower that cannot path must never be
// able to freeze the run.
inline constexpr uint32 ScriptedPullTravelBudgetMs(float yards)
{
    float const travelMs =
        (yards > 0.0f ? yards : 0.0f) / DC_SCRIPTED_PULL_TRAVEL_YD_PER_SEC * 1000.0f;
    return DC_SCRIPTED_PULL_TRAVEL_BASE_MS + static_cast<uint32>(travelMs);
}

// The FOLLOWERS' leash. Tighter than the tank's: the tank plants ON the camp and
// the pack piles onto it there, so a melee follower needs its fuzzed slot offset
// plus melee reach and no more. Every extra yard is drift the follower spends
// "parked" — yielding the tick to whatever is carrying it — before anything
// objects.
inline constexpr float  DC_SCRIPTED_PULL_FOLLOWER_LEASH = 8.0f;

// --- the losing-ground ratchet -------------------------------------------------
// How much ground a leg that should only ever CLOSE may lose against its own
// best-so-far before it is re-issued from scratch. Giving ground is proof something
// else owns the bot's movement (a route spline the pull did not cancel, a chase, a
// knockback). Slack enough to ignore path noise and the arc around a doorway, tight
// enough that an outward excursion is caught within a second rather than after four.
inline constexpr float  DC_SCRIPTED_PULL_LOSE_GROUND = 2.5f;

// Has such a leg lost ground? `bestSoFar` is 0 when no leg is in flight.
//
// THREE legs need this and each one had to be found the hard way, so it is one
// predicate now rather than a fourth copy of the comparison:
//   * the tank's DRAG-BACK          (DcPullContext::scriptedReturnBest)
//   * the FOLLOWER hold-at-camp     (DcPullContext::campHoldBest)
//   * the tank's CAMP LEASH recall  (DcPullContext::scriptedRecallBest)
//
// They share a cause, not just a shape. DcMoveTo DEDUPES on destination, and all
// three re-issue one unchanged point every tick — so the moment another generator
// takes the bot, the leg reports "already going there", issues nothing, and goes
// SILENT. It is not refused, so nothing is logged; and the standing-still backstops
// cannot see it either, because the bot is moving perfectly well, just outward. That
// leaves distance as the only available evidence, which is what this reads.
inline constexpr bool ScriptedPullLostGround(float bestSoFar, float now)
{
    return bestSoFar > 0.0f && now > bestSoFar + DC_SCRIPTED_PULL_LOSE_GROUND;
}

// --- the camp fight's only clock -----------------------------------------
// Every OTHER leg of a scripted pull carries a watchdog; Engage carried none. It
// retires on one predicate — "is any member of this pack still on the party's
// attacker lists" — and if that predicate never goes false the stage never retires,
// which does not merely delay the run but wedges it: a latched stage pins the pull
// target, force-enables the pipeline, holds the party at the camp and stands the
// advance rung down. Nothing downstream can break that; the phase is the only place
// it can be broken.
//
// Live (tr-20260803-144046-4): the tank reached the camp, logged "back on the camp
// (4.5yd) -> fighting", and then said NOTHING for four minutes and thirteen seconds
// — Engage for 254s with three members combat-flagged, every victim empty and every
// health bar at 100%. The pack was on somebody's attacker list and nothing was
// happening. The run was still in that state when it was killed by hand.
//
// PROGRESS, NOT WALL CLOCK. A camp fight's length is set by the pack and the party's
// damage, not by any distance the row authored, so there is no honest budget to size
// it against — and a flat ceiling generous enough for a slow six-mob heroic pack
// (tr-20260803-144046-8 spent 137s on one stage) is too generous to catch a freeze
// quickly. Watching the pack's summed health instead separates the two outright: a
// fight that is happening moves it within seconds, so a legitimate fight of any
// length re-arms the clock over and over and can never trip. Only a fight that has
// actually stopped runs the window down.
//
// 45s of a heroic pack's health not moving by even a percent is not a slow fight, it
// is a stopped one — long enough to ride out a chain-CC or a healer drinking through
// a bad pull, far short of the four minutes this cost.
inline constexpr uint32 DC_SCRIPTED_PULL_ENGAGE_STALL_MS = 45000;

// Has the camp fight stopped happening? `since` is the ms the health signature last
// changed; 0 means "not sampled yet", which can never be stale.
inline constexpr bool ScriptedPullEngageStalled(uint32 since, uint32 nowMs)
{
    return since != 0 && nowMs > since &&
           (nowMs - since) > DC_SCRIPTED_PULL_ENGAGE_STALL_MS;
}

class ScriptedPullRegistry
{
public:
    // --- table access -----------------------------------------------------
    // Cheapest possible gate: most maps have no scripted pull at all and pay one
    // integer compare per call.
    static bool HasRows(uint32 mapId);
    // Every row on `mapId`, ascending by `order`.
    static std::vector<ScriptedPullStage const*> Rows(uint32 mapId);
    // The row with this `order` on this map, or nullptr. `order` is int32 so the
    // "no stage" sentinel (-1, as stored in DcPullContext::scriptedStage) can be
    // passed straight through.
    static ScriptedPullStage const* Find(uint32 mapId, int32 order);

    // --- pure predicates (no world state; unit-tested directly) ------------
    // Is (x,y,z) inside the stage's pack cylinder?
    static bool InPack(ScriptedPullStage const& s, float x, float y, float z);
    // Is `entry` one of the stage's pack members?
    static bool IsPackEntry(ScriptedPullStage const& s, uint32 entry);
    // Is a bot at (x,y,z) close enough to the arm anchor for the stage to arm?
    static bool InArmRange(ScriptedPullStage const& s, float x, float y, float z);
    // Which stage should run? `orders`/`live` are parallel and ascending: `live[i]`
    // is true while stage `orders[i]`'s volume still holds a live pack member.
    // `pinned` is the order of a stage a maneuver is already committed to (-1 for
    // none) and always wins — a stage in flight has DRAGGED its pack out of its own
    // volume, so re-deriving from `live` mid-drag would hand the tank the next pack
    // while it is still hauling this one. Returns the order to run, or -1.
    static int32 SelectOrder(std::vector<uint32> const& orders,
                             std::vector<bool> const& live, int32 pinned);

    // Does `u` belong to `s`'s pack — right entry, standing inside the stage's own
    // volume? The predicate the pull pipeline's positional vetoes except on.
    static bool IsStageTarget(ScriptedPullStage const& s, Unit const* u);

    // --- live resolution --------------------------------------------------
    // The stage the pull pipeline should be running right now, or nullptr. Applies
    // the boss gate, the arm gate, the in-flight pin and SelectOrder.
    //
    // UNCACHED and not cheap — it runs one entry-filtered volume scan per candidate
    // stage. Everything on the per-tick path goes through
    // DcTickMemoAccess::ScriptedStage instead, which memoises this within the tick.
    static ScriptedPullStage const* DueStage(Player* bot, AiObjectContext* ctx);
    // Nearest live, reachable pack member of `s` — the pull target for the stage.
    static Unit* NearestPackMember(Player* bot, AiObjectContext* ctx,
                                   ScriptedPullStage const& s);
};

#endif  // _PLAYERBOT_SCRIPTEDPULLREGISTRY_H
