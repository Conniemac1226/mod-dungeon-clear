/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DUNGEONCLEARMATH_H
#define _PLAYERBOT_DUNGEONCLEARMATH_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Position.h"

namespace DungeonClearMath
{
    // Sentinel "no rejoin crumb" returned by FindTrailRejoin.
    inline constexpr std::size_t TrailRejoinNone = static_cast<std::size_t>(-1);

    // Sentinel "no member needs healing" returned by SelectHealTarget.
    inline constexpr std::size_t HealTargetNone = static_cast<std::size_t>(-1);

    // One candidate heal target for SelectHealTarget. `healthPct` is the member's
    // current health (0..100); `isLeaderTank` marks the elected dungeon-clear
    // leader tank so it can be favoured. Deliberately carries NO line-of-sight or
    // range flag: heal-reposition selection must survive LOS loss (the stock
    // `party member to heal` value drops out-of-LOS members entirely, which is the
    // root of the "healer stops healing when the tank is dragged out of sight"
    // bug). The caller resolves members to candidates from live game state.
    struct HealCandidate
    {
        float healthPct;
        bool  isLeaderTank;
    };

    // Pure heal-target selector (LOS-blind). Among members BELOW `hpFloor` (the
    // "actually needs healing" gate), pick the one with the lowest tank-biased
    // score: `healthPct - (isLeaderTank ? tankBias : 0)`. The bias only breaks the
    // choice toward the tank among members that already need healing — a healthy
    // tank above the floor is never selected — so a hurt tank being kited out of
    // sight is preferred over an equally/slightly-more-hurt DPS, while a healthy
    // tank never steals the pick from a hurt DPS. Returns the chosen index, or
    // HealTargetNone when nobody is below the floor. Separated from the group
    // iteration so the choice is unit-testable.
    std::size_t SelectHealTarget(std::vector<HealCandidate> const& members,
                                 float hpFloor, float tankBias);

    // Pure standoff-point generator. Produces candidate 2D points on a circle of
    // radius `standoffRadius` around `center`, ORDERED so the first is on the side
    // the bot already stands (shortest move / most likely to round the same corner)
    // and the rest fan out alternately to either side. The caller snaps each to the
    // navmesh and keeps the first that has line of sight and a clear path to the
    // center. `ringPoints` extra points are generated past the first (so the result
    // has `ringPoints + 1` entries). Z is left at the center's Z; the caller
    // re-grounds via navmesh snap. With a degenerate bot-on-center input the bias
    // direction falls back to +X. Nothing here is heal-specific: the healer LOS
    // reposition and the contribution-gated combat regroup both sample a role-range
    // ring around a fight anchor with it (see DcActionShared::FindStandoffPoint).
    std::vector<Position> StandoffCandidates(Position const& center,
                                             Position const& bot,
                                             float standoffRadius,
                                             std::uint32_t ringPoints);

    // Deprecated heal-specific alias for StandoffCandidates, kept so the existing
    // heal-reposition call site and t/TestDungeonClearMath / t/TestHealReposition
    // compile unchanged. Prefer StandoffCandidates in new code.
    inline std::vector<Position> HealStandoffCandidates(Position const& target,
                                                        Position const& bot,
                                                        float standoffRadius,
                                                        std::uint32_t ringPoints)
    {
        return StandoffCandidates(target, bot, standoffRadius, ringPoints);
    }

    // One forward hostile for the Dynamic-pull aggro estimate. `chainEligible` is
    // pre-resolved by the caller from game state: true only when this mob is
    // navmesh-reachable from the pull target with a clear line of sight and no
    // closed door between (the far-targets scan ignores LOS, so this gate keeps a
    // mob through a wall / a floor away / behind a door from counting as one that
    // would aggro). It is ignored for mobs that share the target's own pack. `z`
    // carries the mob's world height so the estimate can reject mobs on another
    // floor (a ledge/ramp directly above or below) instead of counting them by
    // plan-view distance alone — see `zTolerance` on EstimateAggroCount.
    //
    // `aggroReach` is this mob's real aggro radius (Creature::GetAggroRange +
    // GetCombatReach, level-diff scaled — see DungeonClearUtil::ClassifyPull-
    // Advanced) measured against the squishiest party member. It is the distance
    // at which the mob proximity-aggros the party fighting at the camp spot, so it
    // self-tunes per zone/level instead of a single hand-tuned chain radius.
    //
    // `packId` is an OPTIONAL engine-pack identity (0 = none) the caller resolves
    // from what actually pulls together: a creature formation group, or a
    // creature_linked_respawn link. Mobs sharing a non-zero `packId` pull as one
    // atomic unit regardless of spacing or height (you cannot pull half a
    // formation), so a counted member drags its whole pack into the estimate.
    // `patroller` marks a DB-authored waypoint mover (GetDefaultMovementType() ==
    // WAYPOINT_MOTION_TYPE). A human tank waits a patrol out instead of committing
    // the heavier pull for a pack that would be a clean small Leeroy moments later;
    // the patrol-wait gate re-runs the estimate with lone patrollers excluded (see
    // `excludeLonePatrollers` on EstimateAggroCount) to detect that case. Random
    // wanderers are deliberately NOT patrollers — their return is unpredictable.
    // `elite` marks an elite-tier body (Creature::isElite(): elite / rare-elite /
    // worldboss rank). It tunes only the WEIGHT a counted mob adds to the pull-
    // weight tally (see EstimateAggroCount's weightThirdsOut), never seeding,
    // assist, or pack membership — a normal mob is a third of an elite because the
    // MaxLeeroyMobs ceiling was set thinking in elites, and a big room of weak
    // trash should not force cautious one-cluster-at-a-time Advanced pulls.
    struct DynPullMob
    {
        float         x;
        float         y;
        float         z;
        bool          chainEligible;
        std::uint32_t packId = 0;
        float         aggroReach = 0.0f;
        bool          patroller = false;
        bool          elite = false;
    };

    // Pure Dynamic-pull estimate: how many mobs aggro if the party Leeroys on top
    // of `mobs[targetIdx]`? The camp spot is the target's position (where a Leeroy
    // fight happens). Returns the estimated body count; the caller compares it to
    // its Leeroy ceiling (count > ceiling => Advanced pull, else Leeroy).
    //   - Seed = the target, its atomic pack (shared non-zero `packId`), and every
    //     other mob that proximity-aggros the camp: within `aggroReach + combat-
    //     Spread` (2D), on the same level (`zTolerance`), and `chainEligible`.
    //     `combatSpread` widens the camp from a point to a disc to model players
    //     drifting to flank/kite during the fight.
    //   - One assist hop: a `chainEligible` mob within `assistRadius` (2D, same
    //     level) of a SEED mob joins via CallForHelp. Proximity aggro from a fixed
    //     camp does not chain, so assisted mobs do NOT seed further proximity or
    //     assist — exactly one ring.
    //   - Formation closure: any pack touched by the set is counted in full.
    // `zTolerance` keeps the estimate honest in multi-level rooms: WotLK inter-
    // floor gaps exceed it, so a mob a ramp above/below never counts. Separated
    // from the game-state resolution in DcPullPlanner::ClassifyPullAdvanced so
    // the logic is unit-testable.
    // `excludeLonePatrollers` (patrol-wait gate): when true, a mob marked
    // `patroller` that is NOT part of an atomic pack (`packId == 0`) is treated as
    // not `chainEligible` — it neither seeds proximity nor assists nor is assisted
    // — modelling "if we let this patrol pass". Formation/linked patrollers
    // (`packId != 0`) are unaffected: you cannot wait out half a formation.
    // `countedOut` (optional) receives the indices of every mob the estimate
    // counted, for diagnostic logging at the call site.
    // `weightThirdsOut` (optional) receives the counted set's PULL WEIGHT, in
    // thirds of an elite: each counted elite adds 3, each counted normal adds 1.
    // This is the value the verdict compares to a x3-scaled ceiling so a normal mob
    // weighs a third of an elite (the ceiling was tuned in elites). The return value
    // stays the raw body count (what `countedOut` enumerates) for diagnostics.
    std::uint32_t EstimateAggroCount(std::vector<DynPullMob> const& mobs,
                                     std::size_t targetIdx, float combatSpread,
                                     float assistRadius, float zTolerance,
                                     bool excludeLonePatrollers = false,
                                     std::vector<std::size_t>* countedOut = nullptr,
                                     std::uint32_t* weightThirdsOut = nullptr);

    // Pull CC-assist grace gate (pure). Decides whether a CC-impaired drag-back
    // should be ABORTED so the party drops passive and piles in to help the tank.
    // `impaired` is the caller's verdict that the leader tank is currently under a
    // pull-ruining control effect (stun / fear / confuse / root / heavy slow).
    // `ccSince` is the timestamp the CURRENT continuous impairment began (0 = not
    // impaired right now). Each tick: while impaired, arm/keep the latch and abort
    // once it has persisted for `graceMs`; while clear, disarm it. A brief micro-CC
    // that clears within the grace therefore never throws the pull away on a
    // flicker, while sustained CC (the pull is failing) releases the party. With
    // `graceMs` == 0 the very first impaired tick aborts. Returns true to abort and
    // writes the updated latch to `ccSinceOut` (which the caller persists in the
    // pull context). Separated from the Unit-state read at the call site so the
    // timing logic is unit-testable.
    bool ShouldAbortPullForCc(bool impaired, std::uint32_t ccSince,
                              std::uint32_t now, std::uint32_t graceMs,
                              std::uint32_t& ccSinceOut);

    // Camp-safety valve (pure). A held passive follower should trigger the valve
    // when it is in combat below `safetyHpPct` AND that has persisted for
    // `graceMs`. `attackerIsPullTarget` is the caller's verdict that everything
    // currently on the follower belongs to the pack being dragged — in which case
    // this is an ordinary drag taking splash, not a failed pull, and the valve
    // must not fire (raw HP% alone is the wrong question: a follower at 60% being
    // splashed by the dragged pack is a normal drag; at 60% from something ELSE, a
    // second pack found us and the maneuver is genuinely compromised).
    // `safetyHpPct` <= 0 disables the valve outright. `since` is the timestamp the
    // CURRENT continuous qualifying spell began (0 = fine); the latch/grace
    // contract mirrors ShouldAbortPullForCc exactly (including the now==0 corner).
    // `graceMs` == 0 fires on the first qualifying tick. Returns true to trip the
    // valve and writes the updated latch to `sinceOut` (persisted in the
    // follower's DcPullContext::campSafetySince by the caller).
    bool ShouldTripCampSafety(bool inCombat, float healthPct, float safetyHpPct,
                              bool attackerIsPullTarget,
                              std::uint32_t since, std::uint32_t now,
                              std::uint32_t graceMs, std::uint32_t& sinceOut);

    // Pull-mode blocking-trash stand-down gate (pure). In advanced-pull mode the
    // pull pipeline owns the pack it is working, so the blocking-trash trigger
    // stands down for it (engage-trash outranks the pull's deliberate wait band
    // and would otherwise preempt every pull). But a BYSTANDER — a pack the pull
    // never selected — appearing inside the aggro-shaped scan while the pull is
    // IDLE is exactly the case the scan exists for, and silently handing it to a
    // pipeline that is not looking at it left the tank walking into it under
    // Advance. Returns true to STAND DOWN (the pull pipeline owns the tick):
    // always for the pull's own pack, and for anyone once a maneuver is in
    // flight (non-Idle — never thrash it). Returns false only for a bystander
    // while the pull is Idle: the blocking-trash walk-in owns that tick. The
    // game-state read (pack identity vs decision/pull target, phase) stays at
    // the trigger; this carries only the decision so it is unit-testable.
    bool ShouldStandDownForPull(bool packIsPullsOwn, bool pullPhaseIdle);

    // Dynamic-verdict drop grace gate (pure). A standing Leeroy/Advanced verdict
    // must survive a TRANSIENT no-target read (door veto flicker, long-path cache
    // mid-rebuild, far-targets poll boundary): dropping it instantly flips the
    // pull-mode bool, releasing the camp hold and stripping daze immunity for a
    // single bad tick, then re-deriving everything — the party lurch. `targetPresent`
    // is the caller's verdict that the pull target resolved this tick. `lostSince`
    // is the timestamp the target first resolved null while the verdict was
    // standing (0 = present). Each tick: while lost, arm/keep the latch and drop
    // once it has persisted for `graceMs`; while present, disarm it. Mirrors
    // ShouldAbortPullForCc's latch/clear contract (including the now==0 corner
    // and graceMs==0 => drop on the first lost tick). Returns true to drop the
    // verdict and writes the updated latch to `lostSinceOut` (persisted in
    // DcPullContext::targetLostSince by the caller).
    bool ShouldDropPullVerdict(bool targetPresent, std::uint32_t lostSince,
                               std::uint32_t now, std::uint32_t graceMs,
                               std::uint32_t& lostSinceOut);

    // Leeroy roll-in gate (pure). True when the party's scout-lag hold should
    // release because the tank is committing to a Leeroy on the verdicted pack:
    // the standing decision is Leeroy (1) and the tank is within
    // `commitRange + lead` (2D) of the live target. `decision` 0 (none, still
    // scouting) and 2 (Advanced — the camp machinery owns the party) never roll
    // in, nor does a dead/unresolvable target. The game-state resolution
    // (decision, live target, distances) stays in
    // DcLeaderSignal::IsLeaderDynamicScouting; this carries only the decision
    // logic so it is unit-testable.
    bool ShouldRollInForLeeroy(std::uint32_t decision, bool targetAlive,
                               float tankToTarget2d, float commitRange, float lead);

    // Patrol-wait gate (pure). A pull is "patrol-contended" when the ONLY thing
    // pushing the aggro estimate over the Leeroy ceiling is one or more lone
    // patrollers: `fullCount > ceiling` but `reducedCount <= ceiling` (the reduced
    // pass excluded them — see EstimateAggroCount's excludeLonePatrollers). For
    // such a pack a human waits the patrol out rather than committing the heavier
    // Advanced maneuver. Returns true to KEEP WAITING (hold the pull), false to
    // proceed (commit the standing verdict). Contended: arm/keep the `waitSince`
    // latch and wait until `waitMs` has elapsed, then proceed with the heavy
    // verdict (don't stall the run for a stationary/slow patrol) — the latch stays
    // armed past the timeout so the wait does not re-fire on the same contention.
    // Not contended (patrol left chain range, or never the cause): clear the latch
    // and proceed at once. `waitMs` == 0 proceeds immediately. Mirrors
    // ShouldAbortPullForCc's by-reference latch/clear contract. The game-state read
    // (the two estimates, the live target/commit distance that decides WHEN to arm)
    // stays in DcPullPlanner::UpdateDynamicPullMode.
    bool ShouldWaitForPatrol(std::uint32_t fullCount, std::uint32_t reducedCount,
                             std::uint32_t ceiling, std::uint32_t waitSince,
                             std::uint32_t now, std::uint32_t waitMs,
                             std::uint32_t& waitSinceOut);

    // Turn-and-plant gate (pure). A human tank dragging a pack back to camp turns
    // and fights the moment the pack is glued to it, rather than sprinting the
    // whole leg back-turned (the single biggest visual bot-tell, and why the
    // daze-immunity cheat exists). True when the drag-back should stop early and
    // the tank turn to fight: the pack is gathered (EVERY live attacker distance
    // <= `glueRadius`) for >= `glueTicksNeeded` consecutive maneuver ticks, this
    // is not an LOS-break pull (`losPull` — those must reach the corner), there is
    // something chasing (empty `attackerDists` => evade/fizzle, never a plant),
    // and at least the first HALF of the return leg is covered
    // (`distToCamp <= legStartDist / 2`, keeping the neighbour-pack clearance the
    // camp was measured for). `plantTicks` is the per-pull debounce latch: it is
    // incremented while the gather condition holds and reset to 0 the moment it
    // breaks, mirroring ShouldAbortPullForCc's by-reference latch contract — so a
    // single-tick noise spike can never trip an early plant. The game-state read
    // (attacker distances, leg progress) stays in DungeonClearPullManeuverAction;
    // this carries only the decision so it is unit-testable.
    bool ShouldPlantEarly(std::vector<float> const& attackerDists, float glueRadius,
                          std::uint32_t glueTicksNeeded, bool losPull,
                          float distToCamp, float legStartDist,
                          std::uint32_t& plantTicks);

    // Threat-lead follower-release gate (pure). After the leader tank enters
    // combat a real group gives it a beat to gather and establish AoE threat
    // before DPS pile in; this holds a follower's fight assist for `leadMs` after
    // the leader's CURRENT combat began (`combatSinceMs`; 0 = leader not in
    // combat). Healers release immediately (`isHealer` — a withheld heal is a
    // wipe and heals don't rip threat the way DPS openers do). DPS release once
    // the lead has elapsed, with THREE bypasses: `alreadyInCombat` (this follower
    // is itself already flagged in combat — it can't over-aggro by moving into
    // sight and MUST be driven onto the tank's fight rather than left with nothing
    // to do, where reviving stock follow-master would drift it to the human), the
    // tank's HP below `panicHpPct` (it is LOSING the fight — pile in; <= 0 disables
    // the bypass), and `leadMs` == 0 (feature off). The game-state read (leader
    // combat stamp, healer role, tank HP, this bot's combat flag) stays in
    // DcLeaderSignal::IsLeaderFightAssistWanted.
    bool ShouldReleaseFollower(bool isHealer, bool alreadyInCombat,
                               std::uint32_t combatSinceMs,
                               std::uint32_t now, std::uint32_t leadMs,
                               float tankHealthPct, float panicHpPct);

    // Phantom-combat classifier (pure). True when a bot is FLAGGED in combat but has
    // nothing it can actually fight from where it stands: nothing is meleeing it
    // (`hasAttacker`), it has no victim of its own (`hasVictim`), and its combat is
    // not explained by any LEGITIMATE holder (`hasLegitimateHolder`). The caller
    // computes the last from the bot's CombatManager PvE refs: a holder counts as
    // legitimate only if it is alive, non-evading, and PATH-REACHABLE from the bot,
    // and an opaque combat with NO unit references at all (a script-forced state) is
    // also treated as legitimate so it is never cleared. This is the signature of the
    // "a mob spawned across the map / behind a gate and tagged me" deadlock: the core
    // combat flag never drops because the holder is unreachable, and every DC gate
    // that keys off "someone is in combat" (the fight-assist arm, the party-engaged
    // latch) then spins forever. Because legitimacy keys on REACHABILITY rather than
    // distance, a fleeing/kiting party (pursuers always reachable) trips
    // `hasLegitimateHolder` and is never phantom — as does any real fight (which
    // trips hasAttacker/hasVictim). The game-state reads stay in
    // DungeonClearBreakStuckCombatTrigger.
    inline bool IsPhantomCombat(bool inCombat, bool hasAttacker, bool hasVictim,
                                bool hasLegitimateHolder)
    {
        return inCombat && !hasAttacker && !hasVictim && !hasLegitimateHolder;
    }

    // "Can this follower attack from where it stands?" for the camp-assist handoff,
    // expressed in the SAME metric the stock reach action enforces.
    //
    // This exists because the two halves of that handoff used different metrics and
    // opened a dead band. Stock ReachSpellAction is constructed with
    // GetRange("spell") but tests it as `!IsWithinCombatRange(target, spellRange)`,
    // and Unit::IsWithinCombatRange adds GetCombatReach(bot) + GetCombatReach(target)
    // to the threshold — so stock stops closing at spellRange + combatReachSum. DC
    // previously required `dist <= spellRange - CONTACT_DISTANCE`, roughly
    // combatReachSum + 0.5 yards TIGHTER than stock would ever walk. A ranged bot
    // left in that window is simultaneously "in range, stop moving" (stock) and "out
    // of range, yield to stock" (DC): neither side acts, the bot never engages, and
    // the tank's spread gate waits on it until the run deadlocks.
    //
    // Ranged therefore mirrors IsWithinCombatRange exactly, making DC's engage window
    // the precise complement of stock's keep-closing window — no gap by construction,
    // and self-syncing if SpellDistance is retuned. Melee passes its own
    // reach-inclusive threshold (reachSum + 1.0), which is already WIDER than stock
    // reach-melee's stop point (reachSum + MeleeDistance, default 0.75) and so
    // overlaps rather than gaps — which is why only ranged ever hung.
    inline bool IsWithinAssistAttackRange(bool isMelee, float dist, float meleeRange,
                                          float spellRange, float combatReachSum)
    {
        return isMelee ? dist <= meleeRange : dist <= spellRange + combatReachSum;
    }

    // Stuck-combat fire gate (pure, streak clock by reference). Given whether the bot
    // is CURRENTLY in phantom combat (IsPhantomCombat above), arm/hold/reset a streak
    // clock and report when the phantom state has persisted continuously for
    // `timeoutMs` — the point at which the caller force-clears combat. `sinceMs` is
    // the caller-owned latch (0 = not streaking): armed to `now` on the first phantom
    // tick, cleared to 0 the instant the phantom state breaks (a real target
    // reappeared), so a one-tick target gap can never trip it. The timeout is
    // deliberately LONG (default 15s) so an ENCOUNTER that intentionally holds the
    // party in combat with no reachable enemy (a scripted wave gap, a gauntlet) is
    // never mistaken for a stuck flag. `timeoutMs == 0` disables the gate. Mirrors
    // ShouldWaitForPatrol's by-reference-latch contract.
    inline bool ShouldBreakStuckCombat(bool phantom, std::uint32_t now,
                                       std::uint32_t timeoutMs, std::uint32_t& sinceMs)
    {
        if (!phantom || timeoutMs == 0)
        {
            sinceMs = 0;
            return false;
        }
        if (sinceMs == 0)
            sinceMs = now ? now : 1;   // arm; avoid the 0 "unarmed" sentinel on ms 0
        // `now >= sinceMs` guards the unsigned subtraction: false only on the ms-0
        // arming tick (sinceMs nudged to 1) or a backward clock step / getMSTime wrap,
        // where "no time has elapsed yet" is the right answer.
        return now >= sinceMs && (now - sinceMs) >= timeoutMs;
    }

    // Bystander-detour borrow watchdog (pure, by-reference latch).
    //
    // Above commit range the tank's approach belongs to Advance (the long-path
    // glide). When a bystander pack's aggro sphere actually sits on the line to
    // the pull target, the pull BORROWS the tick and walks an orbit around it
    // instead — but while it holds the tick, Advance's own wedge/stall ladder is
    // not running, so the borrow must be bounded or a non-converging orbit would
    // freeze the run outright. Bounding it in time is the same "preference, never
    // refusal" rule the detour itself follows: when the borrow stops paying, hand
    // the walk back and let the straight route happen.
    //
    // It is a NO-PROGRESS clock, not a plain deadline: `bestDist` records the
    // closest the tank has been to the pack on this detour, and any tick that
    // beats it by `progressEpsilon` restamps the clock. A long legitimate arc
    // around a big sphere therefore never expires — only an orbit that has
    // genuinely stopped closing burns the budget.
    //
    // `sinceMs == 0` means "not currently borrowing" and arms the clock.
    // `timeoutMs == 0` disables the give-up entirely.
    //
    // The caller owns the latch either side of this: resetting it when the leg's
    // target changes, and deciding what a `false` means. The pull makes it
    // one-shot per pack (DcPullContext::avoidGaveUp) rather than simply retrying
    // once the clock would re-arm — the tank closing one more yard under Advance
    // is enough to satisfy the progress test, so a retrying caller would cancel
    // Advance's spline every fraction of a second and thrash the movement it was
    // supposed to be handing back.
    inline bool ShouldKeepAvoidDetour(std::uint32_t now, float curDist,
                                      std::uint32_t timeoutMs, float progressEpsilon,
                                      std::uint32_t& sinceMs, float& bestDist)
    {
        if (sinceMs == 0 || curDist < bestDist - progressEpsilon)
        {
            sinceMs = now ? now : 1;   // arm; avoid the 0 "unarmed" sentinel on ms 0
            bestDist = curDist;
            return true;
        }
        if (timeoutMs == 0)
            return true;
        // `now >= sinceMs` guards the unsigned subtraction against a backward clock
        // step / getMSTime wrap, where "no time has elapsed" is the right answer.
        return !(now >= sinceMs && (now - sinceMs) >= timeoutMs);
    }

    // --- chase leash -------------------------------------------------------
    // What the walk toward a MOVING trash target should do this tick.
    enum class ChaseVerdict : std::uint32_t
    {
        Follow = 0,   // walk in as planned
        Hold   = 1,   // stand still; let the mob come back to us
        GiveUp = 2,   // the hold ran out — the caller decides what that means
    };

    // Chase-leash gate (pure, by-reference latch).
    //
    // A pull target is latched by GUID and its position is re-read live every
    // tick, so a target that WALKS — a DB patrol, a wanderer, a mob repositioned
    // by its own script — turns every approach into a pursuit. The tank follows it
    // wherever it goes, and when the route it walks passes behind other packs the
    // tank walks through those packs' aggro arcs and arrives at the camp with the
    // whole room. That is a pursuit nobody chose: the pull was planned against the
    // pack's position AT SELECTION TIME (that is what sized the estimate and where
    // the camp was measured from), and the moment the pack leaves that spot the
    // plan is about ground the mob is no longer standing on.
    //
    // A human tank does not chase a patrol. It waits at the spot it picked, and
    // the patrol — by definition a loop — comes back. This is that rule:
    //
    //   `driftFromAnchor` : how far the target has moved from where it stood when
    //                       we committed to it (2D).
    //   `gapAtAnchor`     : distance from the tank's commit spot to that anchor.
    //   `gapNow`          : distance from the SAME commit spot to the target now.
    //                       Measured from the fixed origin, not the tank's live
    //                       position — from a moving origin the gap always shrinks
    //                       as we walk and the leash could never engage.
    //   `destinationHot`  : the target is currently standing inside ANOTHER pack's
    //                       aggro sphere. Reaching it means waking that pack no
    //                       matter how the route bends, so it is never walkable
    //                       ground however small the drift is.
    //
    // Follow while the target is still near where we picked it (`drift <=
    // leashYards`) OR while it has come at least as close to our commit spot as it
    // was then (`gapNow <= gapAtAnchor` — an inbound patrol rounding a wide loop
    // is exactly what we are waiting for, and must not be held on drift alone).
    // Otherwise the target is RECEDING or standing somewhere we must not follow it
    // to: arm the hold latch and stand still until it comes back, and report
    // GiveUp once the hold has burned `holdMs` so no patrol can stall a run.
    //
    // `leashYards` <= 0 disables the gate outright (Follow, latch cleared) —
    // the historical always-chase behaviour, expressible from config.
    // `holdMs` == 0 gives up on the first receding tick (never holds).
    // Latch/clear contract mirrors ShouldWaitForPatrol: the latch stays armed past
    // the timeout, so GiveUp repeats until the caller re-anchors or drops the
    // target rather than silently re-entering a fresh hold.
    inline ChaseVerdict DecideChase(float driftFromAnchor, float gapAtAnchor,
                                    float gapNow, bool destinationHot,
                                    float leashYards, std::uint32_t now,
                                    std::uint32_t holdMs, std::uint32_t& holdSince)
    {
        if (leashYards <= 0.0f)
        {
            holdSince = 0;
            return ChaseVerdict::Follow;
        }
        if (!destinationHot &&
            (driftFromAnchor <= leashYards || gapNow <= gapAtAnchor))
        {
            holdSince = 0;
            return ChaseVerdict::Follow;
        }
        if (holdMs == 0)
            return ChaseVerdict::GiveUp;
        if (holdSince == 0)
            holdSince = now ? now : 1;   // arm; avoid the 0 "unarmed" sentinel on ms 0
        // `now >= holdSince` guards the unsigned subtraction against a backward
        // clock step / getMSTime wrap, where "no time has elapsed" is right.
        if (now >= holdSince && (now - holdSince) >= holdMs)
            return ChaseVerdict::GiveUp;
        return ChaseVerdict::Hold;
    }

    // Engage-fizzle handoff latch (pure). An advanced-pull "camp fight" ended with
    // the tank out of combat but the pulled pack still ALIVE and IDLE — the drag
    // fizzled (a planted caster evaded home the moment the tank broke LOS at camp).
    // Re-pulling repeats the exact same fizzle, so after `maxFizzles` consecutive
    // fizzles of the same pack the caller hands it to the normal walk-in engage
    // instead. `pulledAliveIdle` is the caller's verdict that the pull target
    // resolved and is alive & out of combat (a fizzle this tick). `sameTarget` is
    // true when this fizzle is of the SAME pack the latch already holds (compared
    // against the OLD latched target, before the caller re-stamps it). On a fizzle
    // of a new pack the count restarts at 1; a pack that died or is still being
    // fought (`pulledAliveIdle == false`) clears the count to 0. `fizzleCount` is
    // the by-reference latch (mirrors ShouldWaitForPatrol's contract) and is left
    // holding the updated consecutive-fizzle count for the caller's diagnostics.
    // Returns true when the pack should be handed off (count reached `maxFizzles`).
    // The game-state read (guid identity, alive/combat) and the fizzleTarget guid
    // bookkeeping stay in DungeonClearPullAction's Engage-cleanup branch.
    bool ShouldHandoffFizzledPull(bool pulledAliveIdle, bool sameTarget,
                                  std::uint32_t maxFizzles, std::uint32_t& fizzleCount);

    // Squared 2D distance from point P to segment (A,B).
    float DistSqToSegment2D(float px, float py,
                            float ax, float ay,
                            float bx, float by);

    // True if the 2D segment (A,B) intersects the axis-aligned box
    // [minX,maxX] x [minY,maxY]. Liang-Barsky slab clip: returns true even
    // when BOTH endpoints lie outside the box but the segment passes through
    // it (the case that matters for a thin door panel a path step straddles).
    bool SegmentIntersectsAABB2D(float ax, float ay, float bx, float by,
                                 float minX, float minY,
                                 float maxX, float maxY);

    // Index of the LATEST crumb within `rejoinRadius` (3D) of `cur`, or
    // TrailRejoinNone if none qualifies. Used by the breadcrumb recorder: on a
    // >kJump discontinuity (a drag-back / drop-down), rather than wiping the
    // whole trail, rejoin at the most recent crumb near where the bot now stands
    // and truncate everything ahead of it. Latest-wins so a trail that loops near
    // itself rejoins at the most recent pass, keeping the walked-distance
    // semantics the camp walk-back relies on intact. 3D on purpose — a crumb
    // directly above/below (different floor) must not count as a rejoin.
    std::size_t FindTrailRejoin(std::vector<Position> const& crumbs,
                                Position const& cur, float rejoinRadius);

    // The one shared breadcrumb-trail walk-back primitive (Kernel A). One step of
    // a backward walk, produced by WalkTrailBack for each crumb newest -> oldest.
    // `index` is the crumb's index in the source vector. `along` is the
    // accumulated 3D walked distance from the anchor to `crumb` (the far, away-
    // from-anchor end of this segment); `alongPrev` is that distance at the near
    // end `segStart`. `PointAt(d)` returns the point exactly `d` yards back along
    // THIS segment (linear interpolation, `d` clamped to [alongPrev, along]) — the
    // ce2e89e scout-lag interpolation fix, now available to every walk-back caller
    // instead of one. Positions carry Z, so the walk's jump guard (see
    // WalkTrailBack) and any distance test the caller runs off these fields catch a
    // vertical seam a 2D measure would miss (the seam-undermap / trail-dance bug
    // class: "the arrived? test and the target must share a metric").
    struct TrailStep
    {
        std::size_t index;
        float       alongPrev;
        float       along;
        Position    segStart;   // near end (toward the anchor)
        Position    crumb;      // this crumb (far end, away from the anchor)

        Position PointAt(float d) const
        {
            float const span = along - alongPrev;   // == this segment's 3D length
            if (span <= 0.0001f)
                return crumb;
            float t = (d - alongPrev) / span;
            if (t < 0.0f)
                t = 0.0f;
            if (t > 1.0f)
                t = 1.0f;
            return Position(
                segStart.GetPositionX() + (crumb.GetPositionX() - segStart.GetPositionX()) * t,
                segStart.GetPositionY() + (crumb.GetPositionY() - segStart.GetPositionY()) * t,
                segStart.GetPositionZ() + (crumb.GetPositionZ() - segStart.GetPositionZ()) * t);
        }
    };

    // Walk `crumbs` newest -> oldest starting from `anchor`, accumulating 3D
    // segment length, and call `visit(step)` for each crumb. Stop when a segment
    // exceeds `jumpGuard`: a gap that large is a drag / teleport / drop-down seam,
    // and nothing beyond it is contiguously "behind" the anchor (measuring the
    // segment in 3D makes the guard catch a vertical drop a 2D measure treats as
    // contiguous — the "camp/trail lands on the wrong floor" bug). `visit` returns
    // false to stop the walk early (e.g. the caller accepted a point). Returns the
    // total distance walked (the last visited step's `along`, or 0 for an empty /
    // immediately-discontinuous trail). This is the single primitive behind
    // ComputeSafeCamp, ComputeTrailCamp, GetLeaderScoutTrailPoint and
    // GetLeaderScoutTrail; each supplies its own acceptance predicate (which may
    // probe reachability off the map) as `visit`, so the accounting, the jump
    // guard, and the interpolation live in exactly one tested place.
    template <class Visit>
    inline float WalkTrailBack(std::vector<Position> const& crumbs, Position const& anchor,
                               float jumpGuard, Visit&& visit)
    {
        Position prev = anchor;
        float along = 0.0f;
        for (std::size_t i = crumbs.size(); i-- > 0; )
        {
            Position const& c = crumbs[i];
            float const seg = prev.GetExactDist(&c);
            Position const segStart = prev;
            prev = c;
            if (seg > jumpGuard)
                break;
            float const alongPrev = along;
            along += seg;
            TrailStep const step{ i, alongPrev, along, segStart, c };
            if (!visit(step))
                break;
        }
        return along;
    }

    // The shared jump-guard threshold for WalkTrailBack: a 3D segment longer than
    // this is a trail discontinuity. Was re-declared as a local `kJumpGuard = 12`
    // in each of the four walk-back clones; hoisted here so the four agree by
    // construction.
    inline constexpr float TrailJumpGuard = 12.0f;
}

#endif
