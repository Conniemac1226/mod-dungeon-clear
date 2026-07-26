/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearActions.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Creature.h"
#include "CreatureAI.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSplineInitArgs.h"
#include "ObjectAccessor.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Position.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "Ai/Dungeon/DungeonClear/DcApproachState.h"
#include "Ai/Dungeon/DungeonClear/Data/BossPullbackRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcEventDoorRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearApproach.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearMath.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearApproachIo.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonClearRouteRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/ObjectiveHookRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DcDoorPolicy.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/NavmeshSnap.h"
#include "Ai/Dungeon/DungeonClear/Util/DcPathWorker.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTickMemo.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearTuning.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Ai/Dungeon/DungeonClear/Util/LongRangePathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/StridedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/SwimPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearStateValues.h"
#include "Playerbots.h"
#include "DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"

using namespace DcActionShared;


// --- Advanced pulls -------------------------------------------------------
//
// The maneuver, top to bottom (leader-owned phase, read cross-bot by followers):
//
//   Idle      The tank glides the route normally. When a pullable pack comes
//             within DC_PULL_START_RANGE, pick a SAFE camp back along the already
//             -cleared route (ComputeSafeCamp keeps it clear of other packs and
//             bounds the drag), stamp it, halt the escort glide, -> Forming.
//   Forming   The tank holds where it committed (just outside aggro); the party
//             walks back to the camp and goes passive (hold-at-camp). The tank
//             waits here until the party is actually set at camp (or a timeout),
//             then -> Advancing. The party — not the tank — does the repositioning.
//   Advancing The tank tags the pack: a ranged class pull if it has one and is in
//             range+LOS, else it closes to body-tag. The moment combat starts,
//             control passes to DungeonClearPullManeuverAction on the combat engine.
//   Returning (combat engine) Drag the pack back to the waiting party at camp.
//   Engage    At camp: hand the fight to stock combat; ReapStrandedPassives sees
//             the non-holding phase and releases the party. Out of combat -> Idle.
//
// Camp is anchored in cleared ground BEHIND the tank (where the party already
// trails) rather than at the tank's forward commit spot, and is chosen so the
// fight can't aggro a neighbouring pack — so the tank pulls TOWARD the party
// instead of the party running out to the tank.
namespace
{
    // STATIC FALLBACK commit range (DC_PULL_START_RANGE) — the distance at which
    // the tank stops gliding, holds, and waits in Forming for the party to set at
    // the camp BEFORE it steps in to tag. The live value is normally
    // DcEngageGeometry::PullCommitRange, sized to the pack's REAL aggro radius so
    // the tank Forms just outside where it would face-pull; this constant only
    // applies when DynamicAggroRange is off or the target isn't a creature. It
    // still sits outside a same-level mob's ~20yd aggro so even the fallback
    // doesn't face-pull. Until the pack is this close Advance glides the tank in
    // (blocking-trash stands down in pull mode so it can't engage first). Defined
    // once in DungeonClearTuning.h (shared with the trigger).
    // Per-leg watchdog (tag / return) — abort a leg that makes no progress so a
    // navmesh wedge can never freeze the pull.
    constexpr uint32 DC_PULL_LEG_TIMEOUT_MS = 10000;
    // Per-leg watchdog for a PULL-BACK boss drag. The flat 10s above is sized to a
    // trash pull, whose legs are a few yards — the tank commits just outside the
    // pack's aggro. A BossPullbackRegistry drag is a different scale: Ghaz'an's
    // anchor is ~150yd of PATH from him (~54yd straight-line), most of it swum out
    // and then run back up a ramp, so a flat 10s would declare the tag leg wedged
    // before the tank had even reached the water.
    //
    // Budget = straight-line leg * kPathDetourFactor (the anchor-to-boss route is
    // ~2.7x its straight line here; 2.5 is the conservative round number) at
    // kDragYardsPerSec, plus the flat timeout as slack. Deliberately still BOUNDED:
    // a genuinely wedged drag must fall out to "fight in place" rather than freeze
    // the run, which is why this is not simply disabled for a pull-back.
    constexpr float DC_PULLBACK_PATH_DETOUR = 2.5f;
    constexpr float DC_PULLBACK_YARDS_PER_SEC = 4.0f;   // pessimistic: swimming + snares
    uint32 DcPullLegTimeoutMs(DcPullContext const& pull, float legDist)
    {
        if (!pull.bossPullback)
            return DC_PULL_LEG_TIMEOUT_MS;
        float const travel = std::max(0.0f, legDist) * DC_PULLBACK_PATH_DETOUR;
        uint32 const budget =
            static_cast<uint32>((travel / DC_PULLBACK_YARDS_PER_SEC) * 1000.0f) +
            DC_PULL_LEG_TIMEOUT_MS;
        return std::max(DC_PULL_LEG_TIMEOUT_MS, budget);
    }

    // Put the tank on a boss's threat list — ONE DIRECTION ONLY: the boss attacks
    // the tank, never the tank attacks the boss.
    //
    // That asymmetry is the whole point and it is load-bearing. The obvious-looking
    // other half — bot->Attack(boss) + ChangeEngine(BOT_STATE_COMBAT), which is what
    // EngageDirect and the Black Morass driver do — gives the TANK a victim, and
    // stock combat's MoveChase then drives it AT the boss. For Ghaz'an that means
    // straight into the lake, with the party following. This is not theoretical: it
    // is exactly what an earlier version of this did.
    //
    // So we only ever ask the CREATURE to come to US. Aggro alone flags the tank
    // into combat, which is all the maneuver needs — the drag-back leg then owns the
    // tank's movement at MOVEMENT_COMBAT priority and runs it home to the camp,
    // outranking any chase stock combat may later decide on.
    //
    // PER-ENCOUNTER OPT-IN. The caller only reaches this when the boss's own
    // registry row carries a positive forceAggroRange, which today is exactly one
    // row. Forcing bypasses the boss's aggro logic — normal, tuned behaviour that
    // should be left alone almost everywhere — so it is reserved for a boss that
    // cannot be tagged the ordinary way at all. See BossPullback::forceAggroRange.
    //
    // Ghaz'an is that boss. His script walks him a waypoint lap and parks him on a
    // platform which — along with the pipe leading to it — is MISSING from the
    // extracted navmesh, and he does not reliably finish the lap, so he is often
    // still out in the water when the party is ready. Either way there is no
    // reachable spot inside his aggro bubble: natural aggro can never fire, and the
    // tag leg could only burn its watchdog holding for it before the run stalled.
    //
    // Idempotent: no-ops once he is already in combat. Returns true when the boss
    // is (now) engaged on us.
    bool DcForceBossAggroOnTank(Player* bot, Creature* boss)
    {
        if (!bot || !boss || !boss->IsAlive())
            return false;

        if (!boss->IsInCombat())
        {
            // He may be sitting in the reset/home idle after his waypoint lap;
            // clear it first or the AI settles straight back into it.
            if (boss->IsInEvadeMode())
                boss->ClearUnitState(UNIT_STATE_EVADE);
            // Adds forced threat on the tank (or SetInCombatWith for a threat-less
            // unit) — this is the "get added to his aggro list" half.
            boss->EngageWithTarget(bot);
            // ...and this makes his AI actually pick the tank up and come, rather
            // than merely holding threat while standing still.
            if (boss->AI())
                boss->AI()->AttackStart(bot);
        }

        // Face him so the tank looks like it pulled rather than spinning on the
        // spot, but deliberately NO SetSelection / Attack / engine flip: see above.
        if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, boss))
            ServerFacade::instance().SetFacingTo(bot, boss);

        return boss->IsInCombat();
    }

    // The furthest point along the route toward `boss` that the tank can stand on
    // WITHOUT being in the water — "as close as it can get without getting wet".
    //
    // Derived from the real navmesh route rather than a straight line, because the
    // straight line from the anchor to Ghaz'an cuts through rock: the route runs
    // along the upper ring, down the south-east ramp, and only then meets the
    // water. Walking the route and stopping at the first wet sample lands the tank
    // on the actual shoreline whatever shape it is, and needs no second
    // hand-authored coordinate to maintain.
    //
    // The route is DENSIFIED before testing. PathGenerator returns string-pulled
    // CORNER points, so a single leg can run for tens of yards and dip through
    // water with both of its endpoints dry — testing only the vertices would walk
    // the tank straight through the lake. Sampled every DC_DRY_STANDOFF_STEP yards.
    //
    // Returns nullopt when the route is unusable, or when even the tank's current
    // position is wet (nothing to aim at — the caller keeps what it has). When the
    // whole route is dry it returns the far end, i.e. the boss himself, which is
    // the correct answer for a boss standing on reachable ground.
    constexpr float DC_DRY_STANDOFF_STEP = 3.0f;
    // How far BACK from the water's edge the stand-off is placed. The last dry
    // sample is, by definition, the last spot that is still dry — standing exactly
    // there leaves no room for error, and the tank was observed ending up in the
    // water anyway: the arrival tolerance (DC_PULL_CAMP_ARRIVE) is satisfied
    // anywhere within 5yd of the target, including 5yd PAST it, and a
    // MOVEMENT_COMBAT glide can carry a little further still. Backing the aim point
    // off by more than that tolerance makes the overshoot land on dry ground
    // instead of in the lake. Costs a couple of yards of pull range and nothing
    // else — the force-aggro reaches far past this either way.
    constexpr float DC_DRY_STANDOFF_BACKOFF = 8.0f;

    std::optional<Position> DcDryStandoffToward(Player* bot, Unit* boss)
    {
        if (!bot || !boss)
            return std::nullopt;
        Map* const map = bot->GetMap();
        if (!map)
            return std::nullopt;

        PathGenerator gen(bot);
        gen.CalculatePath(boss->GetPositionX(), boss->GetPositionY(),
                          boss->GetPositionZ(), /*forceDest*/ false);
        Movement::PointsArray const& path = gen.GetPath();
        if (path.size() < 2)
            return std::nullopt;

        uint32 const phase = bot->GetPhaseMask();
        float const collision = bot->GetCollisionHeight();

        // Every dry sample, in route order, so the back-off below can step back
        // along the ROUTE rather than in a straight line — the shoreline here is at
        // the foot of a ramp, and a straight-line back-off would aim into the ramp
        // wall instead of up it.
        std::vector<Position> dry;
        bool hitWater = false;
        for (std::size_t i = 1; i < path.size() && !hitWater; ++i)
        {
            G3D::Vector3 const& a = path[i - 1];
            G3D::Vector3 const& b = path[i];
            float const legLen = (b - a).length();
            uint32 const steps =
                std::max(1u, static_cast<uint32>(std::ceil(legLen / DC_DRY_STANDOFF_STEP)));
            for (uint32 s = 1; s <= steps; ++s)
            {
                float const t = static_cast<float>(s) / static_cast<float>(steps);
                float const x = a.x + (b.x - a.x) * t;
                float const y = a.y + (b.y - a.y) * t;
                float const z = a.z + (b.z - a.z) * t;
                if (map->IsInWater(phase, x, y, z, collision))
                {
                    hitWater = true;    // first wet sample — the route ends here
                    break;
                }
                dry.push_back(Position(x, y, z));
            }
        }

        if (dry.empty())
            return std::nullopt;

        // Whole route dry (the boss is on ground we can walk to): aim at the far
        // end, i.e. him. No back-off — there is no water to keep clear of.
        if (!hitWater)
            return dry.back();

        // Water ahead: walk back from the edge along the samples we just took until
        // we have DC_DRY_STANDOFF_BACKOFF yards of margin. If the dry stretch is
        // shorter than the back-off, the very first dry sample is the best we have.
        float back = 0.0f;
        for (std::size_t i = dry.size(); i-- > 1;)
        {
            back += dry[i].GetExactDist(&dry[i - 1]);
            if (back >= DC_DRY_STANDOFF_BACKOFF)
                return dry[i - 1];
        }
        return dry.front();
    }

    // Consecutive fizzled pulls of the SAME pack (Engage cleanup found the pull
    // target alive and idle — the drag never delivered it) before the pack is
    // handed to the normal walk-in engage via abortTarget. Casters and planted
    // stragglers ignore the drag-back, evade home once the tank breaks LOS at
    // camp, and a re-pull just repeats the fizzle — the tank bouncing forward
    // and back while the party stands at camp never entering combat.
    constexpr uint32 DC_PULL_FIZZLE_MAX = 2;
    // Longest the tank waits in Forming for the party to park+go passive at camp
    // before tagging anyway. Sized to cover the party walking back to a far camp
    // (PullSetback can be tens of yards); past it the tank tags regardless and the
    // party finishes converging on camp during the drag-back. The between-pulls
    // gate already ensured the party was close and rested before the pull began.
    constexpr uint32 DC_PULL_PARTY_SET_TIMEOUT_MS = 8000;
    // How close to camp the tank must get on the return before releasing.
    constexpr float DC_PULL_CAMP_ARRIVE = 5.0f;
    // ...and, for a PULL-BACK only, how close the BOSS must also be before the
    // party is released. The tank gets home long before a boss it pulled from the
    // water's edge, and releasing on tank-arrival alone sent the whole party
    // sprinting at the inbound boss (into the lake). Sized a little over melee
    // range plus the camp slot spread, so the release lands as he closes on the
    // anchor rather than after he is already swinging.
    constexpr float DC_PULLBACK_RELEASE_RANGE = 18.0f;
    // Where a summoned pull-back boss lands, measured out from the anchor toward
    // the tank. Far enough that he does not materialise inside the party (a
    // knockback or cleave landing on everyone at once), close enough to be inside
    // the release range so the fight starts the same tick.
    constexpr float DC_PULLBACK_SUMMON_OFFSET = 6.0f;
    static_assert(DC_PULLBACK_SUMMON_OFFSET < DC_PULLBACK_RELEASE_RANGE,
                  "a summoned boss must land inside the release range, else the "
                  "party is held at camp with the boss already on top of them");
    // "Party is set" tolerance for the Forming gate. A touch wider than the hold
    // radius so a follower parked at the boundary reliably counts as set instead
    // of flickering in/out and never letting the tank tag.
    constexpr float DC_PULL_SET_RADIUS = 8.0f;

    // Resolve the advanced-pull camp params (setback / camp-clear radius / drag cap)
    // from the run settings, widening the clearance and drag cap to the boss's skirt
    // when a room-aggro pre-clear with a skirt override is active (Sepethrea). Without
    // this the camp only clears the boss by the generic PullCampSafeRadius (~25yd),
    // which can land inside her wider aggro/CallForHelp when a pack kept on her edge
    // (pullOutRadius) is dragged just far enough to clear other packs. Raising the drag
    // cap alongside lets the drag actually reach the wider camp. Everywhere else the
    // skirt is 0 and the settings pass through unchanged.
    void DcResolveCampParams(Player* bot, AiObjectContext* ctx,
                             float& setback, float& safeRadius, float& maxDrag)
    {
        setback = DcSettings::GetFloat(bot, "PullSetback");
        safeRadius = DcSettings::GetFloat(bot, "PullCampSafeRadius");
        maxDrag = DcSettings::GetFloat(bot, "PullMaxDrag");
        float const skirt = DcTargeting::ActiveRoomSkirt(bot, ctx);
        if (skirt <= 0.0f)
            return;
        safeRadius = std::max(safeRadius, skirt);
        maxDrag = std::max(maxDrag, skirt + DcSettings::GetFloat(bot, "RoomAggroPartyMargin"));
    }

    // Thin context adapter: resolves the pull-context value and delegates every
    // phase write to DcPullContext::Transition, where the Engage special-case and
    // the transition invariants live. No phase-write logic remains in this TU.
    void DcSetPullPhase(AiObjectContext* context, DcPullPhase p)
    {
        DcPullContext& pull = context->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
        pull.Transition(p, getMSTime());
    }

    // Why the leader tank can't carry out the pull drag-back right now, or nullptr
    // if it is fine. Hard loss-of-control (stun / fear / confuse) and root stop the
    // retreat outright; a heavy movement slow (run speed at/below `slowFloor` of
    // base) drags so slowly the pack wins the race home. Daze is already immunized
    // for the pull (DcLeaderSignal::SetLeaderDazeImmunity), so a slow seen here is a
    // genuine debuff — Hamstring, web, a Frostbolt chill. Read purely off Unit state
    // so no aura-id table is needed; the timing/grace decision lives in the
    // unit-tested DungeonClearMath::ShouldAbortPullForCc. The returned string is a
    // stable literal safe to log.
    char const* DcDragImpairReason(Player* tank, float slowFloor)
    {
        if (!tank)
            return nullptr;
        if (tank->HasUnitState(UNIT_STATE_STUNNED))
            return "stunned";
        if (tank->HasUnitState(UNIT_STATE_FLEEING))
            return "feared";
        if (tank->HasUnitState(UNIT_STATE_CONFUSED))
            return "confused";
        if (tank->IsRooted() || tank->HasUnitState(UNIT_STATE_ROOT))
            return "rooted";
        if (tank->GetSpeedRate(MOVE_RUN) <= slowFloor)
            return "slowed";
        return nullptr;
    }
}


bool DungeonClearPullAction::Execute(Event /*event*/)
{
    DcPullContext& pull = context->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
    Position& camp = pull.camp;
    uint32 const since = pull.phaseSince;
    uint32 const now = getMSTime();
    DcPullPhase const phase = pull.phase;

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, DcKey::NextDungeonBoss);

    switch (phase)
    {
        case DcPullPhase::Idle:
        {
            // Begin a pull: choose a SAFE camp back along the cleared route, hold
            // here, and let the party reposition to it (the tank doesn't retreat).
            Unit* trash = next.has_value()
                ? DcTargeting::GetPullTarget(botAI) : nullptr;
            if (!trash)
            {
                DC_PULL_TRACE("[DC:{}] pull idle: no pull target -> yield to advance",
                              bot->GetName());
                return false;
            }

            // PULL-BACK boss (BossPullbackRegistry). Two things differ from a
            // trash pull and both are structural, not tuning:
            //
            //  * The camp is the hand-authored ANCHOR, never ComputeSafeCamp's
            //    trail-derived point. The anchor was chosen because it is the
            //    nearest ground the party can survive on; a trail camp would be
            //    "PullSetback yards back from wherever the tank is", which is not
            //    the same place and carries no such guarantee.
            //  * There is no glide-closer-to-commit phase. The tank is ALREADY on
            //    the anchor — arriving there is what armed this pull — and Advance
            //    deliberately routes to the anchor and never at the boss, so
            //    yielding to it would just stand still forever. Commit now; the
            //    Advancing leg is what covers the (long) distance out to the boss.
            if (BossPullback const* pb =
                    BossPullbackRegistry::Find(bot->GetMapId(), trash->GetEntry()))
            {
                pull.PublishCamp(Position(pb->campX, pb->campY, pb->campZ), now);
                pull.bossPullback = true;
                pull.losPull = false;   // not an LOS-break pull; distinct suppression
                pull.pullTarget = trash->GetGUID();
                DcMovement::StopBot(bot, DcMovement::Stop::Hold);
                DcFaceIfNeeded(bot, trash);
                DcSetPullPhase(context, DcPullPhase::Forming);
                DC_PULL_INFO("[DC:{}] pull-back plan: boss {} at {:.1f}yd | anchor "
                             "({:.1f},{:.1f},{:.1f}) -> forming, waiting for party to "
                             "set on safe ground",
                             bot->GetName(), trash->GetGUID().ToString(),
                             bot->GetExactDist(trash), pb->campX, pb->campY, pb->campZ);
                return true;
            }

            // Patrol-wait hold (decision == 3): the governor has the pull held at
            // commit range to let a patrol clear before committing. Halt and own the
            // tick — the blocking/room-trash engages already stood down for decision
            // 3, so this just keeps the tank planted (no tag, no camp handshake)
            // until the governor flips the verdict (patrol passed -> LEEROY/ADVANCED)
            // or the wait times out. Followers trail normally (pull mode is off).
            if (pull.decision == DcPullDecisionCode::PatrolHold)
            {
                // Hold, not Soft: this branch is only reached at commit range,
                // i.e. the tank arrives here mid escort-spline glide driven by
                // Advance. Stop::Soft is not escort-aware and lets the tank coast
                // on into the pack it is meant to be waiting out; Stop::Hold kills
                // the glide (same reason the commit branch below uses Hold).
                DcMovement::StopBot(bot, DcMovement::Stop::Hold);
                DC_PULL_TRACE("[DC:{}] pull idle: holding for patrol ({:.1f}yd to pack)",
                              bot->GetName(), bot->GetExactDist2d(trash));
                return true;
            }
            // Don't commit until the pack is within reach (the trigger gates on the
            // same range). While it's farther, yield to Advance to glide closer —
            // but PUBLISH a prospective camp NOW so the party walks up to it IN
            // PARALLEL with the tank's approach, instead of holding at the stale
            // camp until the tank arrives and only THEN trudging forward (the old
            // robotic "tank scouts, wait, party moves, wait" stall the player saw).
            //
            // ComputeSafeCamp returns a point `setback` behind the tank along the
            // breadcrumb trail; as the tank glides in it creeps forward, so the
            // party trails at a roughly fixed standoff. We only ADOPT a candidate
            // that sits closer to the pack than the current camp (with a little
            // hysteresis), so the party advances monotonically and is never dragged
            // backward early in the glide — when "setback behind the tank" can still
            // fall behind the previous/seed camp. We still yield (return false) so
            // Advance keeps gliding the tank; the camp update is a pure side effect.
            float const toTrash = bot->GetExactDist2d(trash);
            // Commit only once the pack is within its OWN aggro range (+ margin), so
            // the tank stops to Form just outside where it would otherwise face-pull.
            float const commitRange =
                DcEngageGeometry::PullCommitRange(bot, trash, DC_PULL_START_RANGE);
            if (toTrash > commitRange)
            {
                // Room-wide-aggro pre-clear (RoomAggroRegistry): the pull is aimed
                // at a ROOM mob near a flagged boss, not a corridor pack. Yielding
                // to Advance here would glide the tank toward the BOSS and wake it
                // — exactly what the pre-clear exists to prevent. Instead publish a
                // prospective camp (so the party trails up in parallel) and walk
                // straight to the room-trash unit until it is within commit range,
                // then fall through to the normal Forming handshake below.
                if (DcTargeting::IsRoomClearActive(bot, context))
                {
                    float sb = 0.0f, sr = 0.0f, md = 0.0f;
                    DcResolveCampParams(bot, context, sb, sr, md);
                    float clr = 0.0f, drg = 0.0f;
                    if (std::optional<Position> ahead = DcPullPlanner::ComputeSafeCamp(
                            botAI, trash, sb, sr, md, clr, drg))
                    {
                        if (!pull.HasCamp() || ahead->GetExactDist2d(trash) + 3.0f <
                                                   camp.GetExactDist2d(trash))
                            pull.PublishCamp(*ahead, now);
                        else
                            pull.TouchCampOwnership(now);
                    }
                    // Walk to the room trash, skirting the boss's aggro sphere if
                    // the direct line clips it (RoomAggroSkirtPoint) — the same
                    // detour EngageDirect applies, so the pull-idle approach can't
                    // wake the boss either.
                    MoveToSkirtingRoomAggro(trash, MovementPriority::MOVEMENT_NORMAL);
                    DC_PULL_TRACE("[DC:{}] pull idle (room-clear): closing on room "
                                  "trash {} at {:.1f}yd (commit {:.1f})",
                                  bot->GetName(), trash->GetGUID().ToString(),
                                  toTrash, commitRange);
                    return true;
                }
                float const setback = DcSettings::GetFloat(bot, "PullSetback");
                float const safeRadius = DcSettings::GetFloat(bot, "PullCampSafeRadius");
                float const maxDrag = DcSettings::GetFloat(bot, "PullMaxDrag");
                float clrAhead = 0.0f;
                float dragAhead = 0.0f;
                if (std::optional<Position> ahead = DcPullPlanner::ComputeSafeCamp(
                        botAI, trash, setback, safeRadius, maxDrag, clrAhead, dragAhead))
                {
                    float const candToTrash = ahead->GetExactDist2d(trash);
                    // +3yd hysteresis: only rewrite when the candidate is meaningfully
                    // more forward, so the party isn't churned by tick-to-tick jitter.
                    // A successful camp computation claims ownership for this
                    // tick even when the hysteresis keeps the old point —
                    // "no change" is still an ownership decision (TouchCampOwnership),
                    // and Advance's scout-trailing must not wrestle the camp meanwhile
                    // (see campPublishedMs / DC_CAMP_PUBLISH_FRESH_MS).
                    if (!pull.HasCamp() || candToTrash + 3.0f < camp.GetExactDist2d(trash))
                    {
                        pull.PublishCamp(*ahead, now);
                        DC_PULL_DEBUG("[DC:{}] pull idle: target {} at {:.1f}yd > start "
                                      "range {:.1f} -> party advances to camp "
                                      "({:.1f},{:.1f},{:.1f}) {:.1f}yd from pack while "
                                      "tank closes", bot->GetName(),
                                      trash->GetGUID().ToString(), toTrash,
                                      commitRange, camp.GetPositionX(),
                                      camp.GetPositionY(), camp.GetPositionZ(),
                                      candToTrash);
                        return false;
                    }
                    // Hysteresis kept the old camp: still assert ownership this tick.
                    pull.TouchCampOwnership(now);
                }
                DC_PULL_TRACE("[DC:{}] pull idle: target {} at {:.1f}yd > start range "
                              "{:.1f} -> glide closer before committing",
                              bot->GetName(), trash->GetGUID().ToString(), toTrash,
                              commitRange);
                return false;
            }
            // Pick the camp: a generous distance back along the cleared route
            // (PullSetback) so the party gets real room, extended further only if
            // another pack is still within PullCampSafeRadius (ComputeSafeCamp).
            // Dungeon mobs have no leash, so far-back is good; PullMaxDrag is just a
            // sanity cap. During a skirt-override room-clear the clear radius and drag
            // cap widen to the boss's skirt (DcResolveCampParams) so the kept-close
            // pack is dragged clear of the boss before the kill.
            float setback = 0.0f, safeRadius = 0.0f, maxDrag = 0.0f;
            DcResolveCampParams(bot, context, setback, safeRadius, maxDrag);
            float clearance = 0.0f;
            float drag = 0.0f;
            std::optional<Position> camped = DcPullPlanner::ComputeSafeCamp(
                botAI, trash, setback, safeRadius, maxDrag, clearance, drag);
            pull.PublishCamp(camped.has_value()
                                 ? *camped
                                 : Position(bot->GetPositionX(), bot->GetPositionY(),
                                            bot->GetPositionZ()),
                             now);

            // Record whether this is a line-of-sight pull (ranged pack, camp placed
            // to break LOS) so the addon status line can announce it. Mirrors the
            // gate inside ComputeSafeCamp.
            pull.losPull = DcSettings::GetBool(bot, "PullRangedLosBreak") &&
                           DcEngageGeometry::IsRangedAttacker(bot, trash);

            // The pack this pull is about. Survives EnterEngage so the Engage
            // cleanup can detect a fizzled drag (target alive + idle after the
            // camp fight) and latch the pack over to the walk-in engage.
            pull.pullTarget = trash->GetGUID();

            // Halt the escort glide for real before committing. A plain StopMoving
            // does NOT cancel a launched escort spline (the door-blocked park is the
            // other witness), so without StopBot(Hold) the tank keeps gliding
            // forward past the commit spot and the camp is stamped behind a
            // position it's already leaving — the old overshoot / wrong-target pull.
            DcMovement::StopBot(bot, DcMovement::Stop::Hold);
            // Square up on the pack for the dwell at the commit spot.
            DcFaceIfNeeded(bot, trash);
            DcSetPullPhase(context, DcPullPhase::Forming);
            // clearance == -1 means no other pack is anywhere near the camp.
            float const clrDisp =
                clearance >= std::numeric_limits<float>::max() ? -1.0f : clearance;
            size_t const trailLen = pull.breadcrumbs.size();
            DC_PULL_INFO("[DC:{}] advanced-pull plan: target {} at {:.1f}yd | camp "
                         "({:.1f},{:.1f},{:.1f}) drag {:.1f}yd | clearance {:.1f}yd "
                         "(safe {:.0f}, setback {:.0f}, trail {}) -> forming, "
                         "waiting for party",
                         bot->GetName(), trash->GetGUID().ToString(), toTrash,
                         camp.GetPositionX(), camp.GetPositionY(), camp.GetPositionZ(),
                         drag, clrDisp, safeRadius, setback, trailLen);
            return true;
        }

        case DcPullPhase::Forming:
        {
            // The tank HOLDS where it committed (just outside aggro). The party
            // walks back to the camp and goes passive (hold-at-camp). Tag only once
            // the party is actually set, so the pull never drags into open ground.
            DcMovement::StopBot(bot, DcMovement::Stop::Soft);
            // Keep facing the pack across the multi-second dwell (idempotent —
            // re-faces only if the pack repositioned us off-axis).
            DcFaceIfNeeded(bot, DcTargeting::GetPullTarget(botAI));

            bool const partySet =
                DcPullPlanner::IsPartySetAtCamp(bot, camp, DC_PULL_SET_RADIUS);
            bool const formingTimedOut = (now - since) > DC_PULL_PARTY_SET_TIMEOUT_MS;
            if (!partySet && !formingTimedOut)
            {
                DC_PULL_TRACE("[DC:{}] pull forming: waiting for party to set at camp "
                              "({}/{} ms)", bot->GetName(), now - since,
                              DC_PULL_PARTY_SET_TIMEOUT_MS);
                return true;
            }
            DcSetPullPhase(context, DcPullPhase::Advancing);
            DC_PULL_DEBUG("[DC:{}] pull forming complete ({}) -> advancing (tag)",
                          bot->GetName(),
                          partySet ? "party set" : "timed out waiting for party");
            return true;
        }

        case DcPullPhase::Advancing:
        {
            // Tag the pack. The moment combat starts the non-combat trigger stops
            // firing and DungeonClearPullManeuverAction drags it back to camp.
            Unit* trash = next.has_value() ? DcTargeting::GetPullTarget(botAI) : nullptr;
            if (!trash)
            {
                // Pack died / despawned before we tagged it — nothing to pull.
                DcSetPullPhase(context, DcPullPhase::Idle);
                DC_PULL_DEBUG("[DC:{}] pull advancing: target gone -> idle",
                              bot->GetName());
                return false;
            }

            // Tag-leg budget: the camp-to-target span, so a pull-back's long haul
            // out to the boss gets a proportionate watchdog (see DcPullLegTimeoutMs).
            if ((now - since) > DcPullLegTimeoutMs(pull, camp.GetExactDist(trash)))
            {
                // Stalled without aggro (e.g. the tag resisted and we never closed).
                // Hand this pack to the normal walk-in engage so the run never hangs.
                pull.abortTarget = trash->GetGUID();
                DcSetPullPhase(context, DcPullPhase::Idle);
                DC_PULL_INFO("[DC:{}] advanced-pull: tag timed out (target {} at "
                             "{:.1f}yd) -> normal engage", bot->GetName(),
                             trash->GetGUID().ToString(), bot->GetExactDist(trash));
                return false;
            }

            // FORCE-AGGRO — per-encounter opt-in, keyed off the boss's own registry
            // row (BossPullback::forceAggroRange > 0), NOT off "this is a pull-back
            // pull". A pull-back boss whose row leaves the field at its 0 default
            // falls straight through to the ordinary tag machinery below, exactly
            // like every other boss in every other dungeon. Today one row opts in.
            //
            // Everything below this point — the ranged tag, the creep to the aggro
            // edge, the hold-for-aggro dwell — assumes the target will notice a tank
            // standing in its bubble. Ghaz'an is the boss that breaks the
            // assumption outright: his platform and its pipe are missing from the
            // navmesh, and he is often still in the water when the party is set, so
            // there is no reachable spot inside his aggro range at all. The tag leg
            // could only burn its watchdog before the run stalled — the reported
            // "tank waits for him until he times out".
            //
            // The shape is: walk out to the water's edge, get on his threat list
            // from there, run home. Three deliberate properties:
            //
            //  * The tank stops at the last DRY point on the route
            //    (DcDryStandoffToward). It goes as close as it can and no closer —
            //    it must never wade in, because the party ends up wherever the tank
            //    takes the fight.
            //  * The aggro is ONE-DIRECTIONAL (DcForceBossAggroOnTank): he attacks
            //    the tank, the tank never attacks him. Giving the tank a victim here
            //    hands it to stock MoveChase, which walks it into the lake with the
            //    party in tow — the observed failure.
            //  * The run home is issued on the SAME tick as the aggro, at
            //    MOVEMENT_COMBAT priority, so the retreat is already in flight
            //    before he arrives and does not wait on the engine flip.
            BossPullback const* const forceRow =
                pull.bossPullback
                    ? BossPullbackRegistry::Find(bot->GetMapId(), trash->GetEntry())
                    : nullptr;
            if (forceRow && forceRow->forceAggroRange > 0.0f)
            {
                float const toBoss = bot->GetExactDist(trash);
                if (toBoss > forceRow->forceAggroRange)
                {
                    // Out of range entirely — fall through to the walk-in below and
                    // force on a later tick.
                    DC_PULL_TRACE("[DC:{}] pull-back: boss at {:.1f}yd > force range "
                                  "{:.0f} -> closing first", bot->GetName(), toBoss,
                                  forceRow->forceAggroRange);
                }
                else
                {
                    // Walk out to the shoreline first, if there is still dry ground
                    // between us and him. `standoff` is the far end of the dry part
                    // of the route (less a back-off margin), so when he is on
                    // reachable ground it simply resolves to him and this
                    // degenerates to a normal walk-in.
                    //
                    // ALREADY WET is checked first and is a hard stop, not a
                    // preference. The stand-off aim point is only ever an aim point:
                    // a glide can overshoot it, the shoreline can move as the boss
                    // does, and the route can change under us. If any of that has
                    // already put the tank in the water then walking further is the
                    // exact failure this branch exists to avoid — pull from where we
                    // stand and get out. (Deliberately no retreat-then-pull dance:
                    // the aggro reaches from here, and the retreat is the very next
                    // thing that happens anyway.)
                    std::optional<Position> const standoff =
                        bot->IsInWater() ? std::nullopt : DcDryStandoffToward(bot, trash);
                    if (bot->IsInWater())
                    {
                        DC_PULL_DEBUG("[DC:{}] pull-back: tank is in the water at the "
                                      "approach — pulling from here rather than "
                                      "wading further", bot->GetName());
                    }
                    if (standoff.has_value() &&
                        bot->GetExactDist(&*standoff) > DC_PULL_CAMP_ARRIVE)
                    {
                        DC_PULL_TRACE("[DC:{}] pull-back: walking to the dry stand-off "
                                      "({:.1f},{:.1f},{:.1f}), {:.1f}yd off, before "
                                      "pulling", bot->GetName(),
                                      standoff->GetPositionX(), standoff->GetPositionY(),
                                      standoff->GetPositionZ(),
                                      bot->GetExactDist(&*standoff));
                        DcMoveTo(bot->GetMapId(), standoff->GetPositionX(),
                                 standoff->GetPositionY(), standoff->GetPositionZ(),
                                 /*idle*/ false, /*react*/ false, /*normal_only*/ false,
                                 /*exact_waypoint*/ false,
                                 MovementPriority::MOVEMENT_COMBAT);
                        return true;
                    }

                    // At the water's edge (or already as close as the route allows):
                    // put ourselves on his threat list and immediately head home.
                    Creature* const bossCreature = trash->ToCreature();
                    if (bossCreature && DcForceBossAggroOnTank(bot, bossCreature))
                    {
                        pull.tagTarget = trash->GetGUID();
                        // Stamp the return leg here rather than waiting for the
                        // maneuver's first combat tick, so the turn-and-plant /
                        // watchdog arithmetic measures the real leg and the retreat
                        // starts THIS tick.
                        pull.returnLegStartDist = bot->GetExactDist(&camp);
                        pull.plantTicks = 0;
                        DcSetPullPhase(context, DcPullPhase::Returning);
                        DC_PULL_INFO("[DC:{}] pull-back: on {}'s threat list from the "
                                     "dry stand-off ({:.1f}yd out) -> running home to "
                                     "the anchor, {:.1f}yd", bot->GetName(),
                                     trash->GetGUID().ToString(), toBoss,
                                     pull.returnLegStartDist);
                        DcMoveTo(bot->GetMapId(), camp.GetPositionX(), camp.GetPositionY(),
                                 camp.GetPositionZ(), /*idle*/ false, /*react*/ false,
                                 /*normal_only*/ false, /*exact_waypoint*/ false,
                                 MovementPriority::MOVEMENT_COMBAT);
                        return true;
                    }
                }
            }

            // Prefer a RANGED tag: pull from spell range so the tank tags and the
            // pack comes to it, instead of running into the middle of the pack.
            ObjectGuid const lastPull = pull.tagTarget;
            if (DC_TRY_PULL_SPELL)
            {
                if (auto pick = ResolvePullSpell(botAI, bot))
                {
                    if (lastPull == trash->GetGUID())
                    {
                        // Already tagged — hold and let aggro flip us to the
                        // combat-engine drag-back. The leg timeout above is the
                        // backstop if the tag somehow drew no aggro.
                        DcMovement::StopBot(bot, DcMovement::Stop::Soft);
                        DcFaceIfNeeded(bot, trash);
                        DC_PULL_TRACE("[DC:{}] pull advancing: tagged, holding for aggro "
                                      "({:.1f}yd to target)", bot->GetName(),
                                      bot->GetExactDist(trash));
                        return true;
                    }
                    float const d = bot->GetExactDist(trash);
                    if (d >= pick->minRange && d <= pick->maxRange &&
                        bot->IsWithinLOSInMap(trash))
                    {
                        bot->SetSelection(trash->GetGUID());
                        if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, trash))
                            ServerFacade::instance().SetFacingTo(bot, trash);
                        if (botAI->CastSpell(pick->spellId, trash))
                        {
                            pull.tagTarget = trash->GetGUID();
                            DcMovement::StopBot(bot, DcMovement::Stop::Soft);
                            DC_PULL_INFO("[DC:{}] advanced-pull: ranged tag spell {} at "
                                         "{:.1f}yd", bot->GetName(), pick->spellId, d);
                            return true;
                        }
                        // Cast failed (cooldown/silence) — fall through to body-tag.
                    }
                    // Not in range/LOS yet — fall through to close the gap; we'll
                    // re-enter and cast once within range and line of sight.
                }
            }

            // No ranged option (or out of range/LOS / cast failed): body-tag by
            // proximity. CRUCIAL: walk only to the EDGE of the pack's aggro bubble
            // and HOLD — let the mob notice and close the last few yards itself —
            // rather than sprinting (COMBAT priority is uninterruptible by combat
            // reflexes) all the way to the pack's centre. The old "MoveTo the mob's
            // exact position" arrived at the spawn before combat even registered, so
            // the drag-back (combat-engine only) took over far too late and an
            // un-trained tank face-pulled the whole pack and ate its opener. This
            // mirrors the ranged "tagged, hold for aggro" path above: stop just
            // inside aggro, let the pack come, then the maneuver drags it to camp.
            Creature* const trashCreature = trash->ToCreature();
            float const toTag = bot->GetExactDist(trash);

            // Distance at which the tank must stop to reliably tag. The core only
            // re-checks a pack's aggro on MOVEMENT (Creature::MoveInLineOfSight,
            // driven by relocation notifiers), and its notice test is the plain
            // CENTER-TO-CENTER distance vs Creature::GetAggroRange (CanStartAttack ->
            // IsWithinDistInMap with BOTH bounding radii excluded). So the tank has
            // to GLIDE to a point strictly INSIDE GetAggroRange — the moving approach
            // is what crosses the threshold and trips the notice. Stop ~2yd inside.
            //
            // The OLD formula ADDED both combat reaches to GetAggroRange, parking the
            // tank ~2yd OUTSIDE the real aggro bubble. Stationary there, no relocation
            // re-fired MoveInLineOfSight, the pack never noticed it, and the leg
            // watchdog timed out — the reported "advanced pull tag timed out" hang.
            float meleeReach = 0.0f;
            float tagStop = 0.0f;
            bool forceTag = false;   // close to body contact and actively swing
            if (trashCreature)
            {
                meleeReach = bot->GetCombatReach() + trash->GetCombatReach() + 1.0f;
                tagStop = trashCreature->GetAggroRange(bot) - 2.0f;
                // Creep the stop point inward the longer we go without aggro. A tank
                // parked exactly at the edge is never re-evaluated (no relocation ->
                // no MoveInLineOfSight), so stepping a little closer each tick is what
                // ultimately trips the notice. Ramps to body contact within ~2s, well
                // inside the leg watchdog, so a borderline stop can't hang the pull.
                uint32 const advancingMs = now - since;
                if (advancingMs > 1500)
                    tagStop -= ((advancingMs - 1500) / 1000.0f) * 3.0f;
                // Floor at body contact. If the pack's aggro is at/below melee (a
                // much-higher-level tank vs the core's 5yd minimum aggro), closing to
                // the edge can't cross it — go to contact and actively tag instead.
                if (tagStop <= meleeReach)
                {
                    tagStop = meleeReach;
                    forceTag = true;
                }
            }

            if (trashCreature && toTag <= tagStop)
            {
                if (forceTag && !bot->IsInCombat())
                {
                    // Pack too high-level to ever notice us on its own: force the tag
                    // with a melee swing so combat starts and the maneuver drag-back
                    // (combat engine) takes over.
                    //
                    // The last two lines are what actually make that sentence true,
                    // and this site was missing both — unlike its two siblings below
                    // (the CC-abort engage and the pack-gathered plant), which carry
                    // the full pattern. Engine transitions in mod-playerbots are
                    // ACTION-DRIVEN, not derived from bot->IsInCombat(): UpdateAI only
                    // auto-switches to/from BOT_STATE_DEAD, and stock reaches the
                    // combat engine solely through AttackAction / PullActions calling
                    // ChangeEngine. Player::Attack alone just hands the bot a victim
                    // and lets the CORE swing it — no AI involvement — so without the
                    // flip the tank stays on the NON-combat engine, where no class
                    // rotation exists to run and DungeonClearPullManeuverTrigger (a
                    // COMBAT-engine trigger) can never fire. The tank would white-hit
                    // the pack in place with no abilities and no drag-back, which is
                    // exactly the "forced attack, but never a real rotation" shape.
                    //
                    // CurrentTarget matters just as much: flipping in with no current
                    // target lets the stock "drop target" action (relevance 99) read
                    // the target as invalid and bounce the bot straight back out — the
                    // 1-tick engine ping-pong DungeonClearMultiplier documents, whose
                    // suppressor is scoped to assisting FOLLOWERS and so does not
                    // cover the leader tank here.
                    //
                    // Narrow by construction: forceTag only when the pack's aggro
                    // range is at or below melee reach, and the branch is gated on
                    // !IsInCombat(), so it fires at most once per pull.
                    bot->SetSelection(trash->GetGUID());
                    if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, trash))
                        ServerFacade::instance().SetFacingTo(bot, trash);
                    context->GetValue<Unit*>(DcKey::Stock::CurrentTarget)->Set(trash);
                    bot->Attack(trash, true);
                    botAI->ChangeEngine(BOT_STATE_COMBAT);
                    DC_PULL_TRACE("[DC:{}] pull advancing: force body-tag ({:.1f}yd)",
                                  bot->GetName(), toTag);
                    return true;
                }
                // Inside the aggro bubble — hold and let the pack close / flip the
                // engine to the maneuver drag-back. The leg timeout above is the
                // backstop if nothing aggros (resisted / non-hostile).
                DcMovement::StopBot(bot, DcMovement::Stop::Soft);
                DcFaceIfNeeded(bot, trash);
                DC_PULL_TRACE("[DC:{}] pull advancing: at aggro edge ({:.1f}yd, "
                              "hold for aggro)", bot->GetName(), toTag);
                return true;
            }

            // Aim for a point tagStop yards out from the pack on the tank's side,
            // not the pack's centre, so the run stops at the aggro edge.
            float tagX = trash->GetPositionX();
            float tagY = trash->GetPositionY();
            float const tagZ = trash->GetPositionZ();
            if (trashCreature && toTag > 0.1f)
            {
                float const f = tagStop / toTag;
                tagX = trash->GetPositionX() + (bot->GetPositionX() - trash->GetPositionX()) * f;
                tagY = trash->GetPositionY() + (bot->GetPositionY() - trash->GetPositionY()) * f;
            }
            DC_PULL_TRACE("[DC:{}] pull advancing: closing to aggro edge ({:.1f}yd, "
                          "stop {:.1f})", bot->GetName(), toTag, tagStop);
            bool const moved = DcMoveTo(trash->GetMapId(), tagX, tagY, tagZ,
                                      /*idle*/ false, /*react*/ false, /*normal_only*/ false,
                                      /*exact_waypoint*/ false, MovementPriority::MOVEMENT_COMBAT);
            if (moved || bot->isMoving() || IsWaitingForLastMove(MovementPriority::MOVEMENT_COMBAT))
                return true;

            // Couldn't move and not moving: navmesh wedge. Abort to normal engage.
            pull.abortTarget = trash->GetGUID();
            DcSetPullPhase(context, DcPullPhase::Idle);
            DC_PULL_INFO("[DC:{}] advanced-pull: tag navmesh-wedged ({:.1f}yd to "
                         "target) -> normal engage", bot->GetName(),
                         bot->GetExactDist(trash));
            return false;
        }

        case DcPullPhase::Returning:
        {
            // Reached here only out of combat (the maneuver runs in combat): the
            // pack leashed / evaded mid-return. Release the party and reset.
            //
            // A PULL-BACK retreat is the one case where "the tank is out of combat"
            // does NOT mean that. Its Returning leg is entered by the tag branch on
            // the same tick it puts the tank on the boss's threat list, and the
            // tank's own combat flag can lag that by a tick — and the boss is a long
            // way off, so nothing is hitting the tank yet either. Releasing the
            // party on that tick would dump them out of the camp hold and send them
            // at an inbound boss, which is the beeline this whole maneuver exists to
            // stop. Keep running home while the BOSS is still engaged; only fall
            // through to the release once he has genuinely dropped combat (a real
            // evade), and let the leg watchdog bound it either way.
            if (pull.bossPullback)
            {
                Unit* const pulled = pull.pullTarget.IsEmpty()
                    ? nullptr : ObjectAccessor::GetUnit(*bot, pull.pullTarget);
                if (pulled && pulled->IsAlive() && pulled->IsInCombat())
                {
                    DcMoveTo(bot->GetMapId(), camp.GetPositionX(), camp.GetPositionY(),
                             camp.GetPositionZ(), /*idle*/ false, /*react*/ false,
                             /*normal_only*/ false, /*exact_waypoint*/ false,
                             MovementPriority::MOVEMENT_COMBAT);
                    DC_PULL_TRACE("[DC:{}] pull-back: retreating to the anchor "
                                  "({:.1f}yd) — boss engaged, combat flag not on us "
                                  "yet", bot->GetName(), bot->GetExactDist(&camp));
                    return true;
                }
            }
            DcSetPullPhase(context, DcPullPhase::Engage);
            DC_PULL_INFO("[DC:{}] advanced-pull: out of combat mid-return (pack "
                         "leashed/evaded) -> release party", bot->GetName());
            return false;
        }

        case DcPullPhase::Engage:
        default:
        {
            // Out-of-combat cleanup: the camp fight is over, ready the next pull.
            pull.abortTarget = ObjectGuid::Empty;

            // Engage-fizzle latch. The fight "ended" (tank out of combat) with
            // the pulled pack still alive and idle: the drag never delivered it
            // — a caster/planted straggler held its ground and evaded home the
            // moment the tank broke LOS at camp. Re-pulling repeats the exact
            // same fizzle (the tank bounces forward and back while the party
            // stands passive at camp, never entering combat), so after
            // DC_PULL_FIZZLE_MAX consecutive fizzles hand the pack to the
            // normal walk-in engage: blocking-trash exempts the abortTarget
            // from pull-mode standdown, the tank fights it where it stands, and
            // the leader-fight assist drives the party in. A pack that died or
            // is still being fought by the party resets the latch.
            Unit* pulled = pull.pullTarget.IsEmpty()
                ? nullptr : ObjectAccessor::GetUnit(*bot, pull.pullTarget);
            bool const aliveIdle = pulled && pulled->IsAlive() && !pulled->IsInCombat();
            // sameTarget compares against the OLD latch before we re-stamp it.
            bool const sameTarget = aliveIdle && pull.fizzleTarget == pull.pullTarget;
            bool const handoff = DungeonClearMath::ShouldHandoffFizzledPull(
                aliveIdle, sameTarget, DC_PULL_FIZZLE_MAX, pull.fizzleCount);
            if (aliveIdle)
            {
                if (!sameTarget)
                    pull.fizzleTarget = pull.pullTarget;
                // Health and distance discriminate WHY it fizzled: 100% health
                // means the pack never engaged (or fully reset behind the LOS
                // corner); reduced health means it fought and silently dropped
                // combat when its target became unreachable.
                if (handoff)
                {
                    pull.abortTarget = pull.fizzleTarget;
                    DC_PULL_INFO("[DC:{}] advanced-pull: pull of {} fizzled {}x "
                                 "(alive and idle after the camp fight, {:.0f}% hp, "
                                 "{:.1f}yd) -> handing to normal engage",
                                 bot->GetName(), pull.fizzleTarget.ToString(),
                                 pull.fizzleCount, pulled->GetHealthPct(),
                                 bot->GetExactDist(pulled));
                }
                else
                    DC_PULL_DEBUG("[DC:{}] advanced-pull: pull of {} fizzled "
                                  "(alive and idle after the camp fight, {:.0f}% hp, "
                                  "{:.1f}yd, {}/{})", bot->GetName(),
                                  pull.pullTarget.ToString(), pulled->GetHealthPct(),
                                  bot->GetExactDist(pulled), pull.fizzleCount,
                                  DC_PULL_FIZZLE_MAX);
            }
            else
                pull.fizzleTarget = ObjectGuid::Empty;  // count cleared by the kernel
            pull.pullTarget = ObjectGuid::Empty;

            // End the pull-back session with the maneuver that opened it. The flag
            // is otherwise cleared only by Reset() — which runs on run teardown, not
            // between pulls — so leaving it set here would keep the WHOLE pull
            // pipeline force-enabled for the rest of the run: every later pack would
            // be camp-pulled even with the player's pull setting Off, and the party
            // would stay camp-held at Ghaz'an's dead anchor. Per-pull flag, per-pull
            // lifetime.
            pull.bossPullback = false;

            DcSetPullPhase(context, DcPullPhase::Idle);
            DC_PULL_DEBUG("[DC:{}] advanced-pull: camp fight done -> idle, ready for "
                          "next pull", bot->GetName());
            return false;
        }
    }
}

bool DungeonClearPullManeuverAction::Execute(Event /*event*/)
{
    DcPullContext& pull = context->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
    Position& camp = pull.camp;
    uint32 const now = getMSTime();
    DcPullPhase const phase = pull.phase;

    // Keep the tank daze-proof for the whole drag. Immunity (set with pull mode)
    // should stop Daze landing at all; strip it here too as a backstop so a
    // retreat is never slowed to a crawl by a hit from behind.
    bot->RemoveAurasDueToSpell(1604);

    // A druid tank must take the run-home hits in bear form, not caster form.
    // Shapeshift is instant and not interrupted by the drag-back run, so refresh
    // it every tick of the maneuver (no-op once shifted / for non-druids).
    DcFollowerLifecycle::EnsureTankBearForm(bot);

    // First combat tick of the pull: aggro confirmed, turn around for camp.
    // Forming counts too — combat can be taken while the tank holds at the commit
    // spot waiting for the party to set (the pack wandered into it / a patrol).
    // Idle counts as well: an unplanned aggro while merely scouting toward the
    // next pack must ALSO retreat to the held party rather than fight in place.
    // Either way we drag back to the camp and release the party there rather than
    // letting the tank solo in place.
    if (phase == DcPullPhase::Advancing || phase == DcPullPhase::Forming ||
        phase == DcPullPhase::Idle)
    {
        // Unplanned aggro while scouting (Idle): stamp a FRESH safe camp just
        // behind the tank (away from the aggressor) and drag the pack THERE. The
        // party then converges on the NEW camp via hold-at-camp — we deliberately
        // do NOT haul the pack all the way back to the stale camp the party still
        // sits at. The fight happens on safe ground near where the aggro started
        // and the party comes up to it, even though it isn't there yet.
        // Forming/Advancing already carry the freshly-committed pull camp.
        if (phase == DcPullPhase::Idle)
        {
            Unit* aggressor = bot->GetVictim();
            for (Unit* a : bot->getAttackers())
            {
                if (!a || !a->IsAlive())
                    continue;
                if (!aggressor ||
                    bot->GetExactDist2d(a) < bot->GetExactDist2d(aggressor))
                    aggressor = a;
            }
            if (aggressor)
            {
                // Same fizzle bookkeeping as a planned pull: if this aggressor
                // evades home mid-drag over and over, the Engage cleanup must
                // see it and eventually hand it to the walk-in engage.
                pull.pullTarget = aggressor->GetGUID();

                float setback = 0.0f, safeRadius = 0.0f, maxDrag = 0.0f;
                DcResolveCampParams(bot, context, setback, safeRadius, maxDrag);
                float clr = 0.0f;
                float drag = 0.0f;
                if (std::optional<Position> fresh = DcPullPlanner::ComputeSafeCamp(
                        botAI, aggressor, setback, safeRadius, maxDrag, clr, drag))
                {
                    pull.PublishCamp(*fresh, now);
                    DC_PULL_INFO("[DC:{}] advanced-pull: unplanned aggro while scouting "
                                 "-> fresh camp ({:.1f},{:.1f},{:.1f}) drag {:.1f}yd, "
                                 "party converges", bot->GetName(),
                                 camp.GetPositionX(), camp.GetPositionY(),
                                 camp.GetPositionZ(), drag);
                }
            }
        }

        // If the camp was never stamped and none could be computed, fall back to
        // fighting in place rather than dragging the pack to the map origin.
        if (!pull.HasCamp())
            pull.PublishCamp(Position(bot->GetPositionX(), bot->GetPositionY(),
                                      bot->GetPositionZ()),
                             now);

        // Stamp the return-leg length and clear the plant latch: the turn-and-plant
        // gate (below) requires at least half of THIS leg covered, and the debounce
        // must start fresh for the new drag.
        pull.returnLegStartDist = bot->GetExactDist(&camp);
        pull.plantTicks = 0;

        DcSetPullPhase(context, DcPullPhase::Returning);
        DC_PULL_INFO("[DC:{}] advanced-pull: aggro confirmed at {:.1f}yd from camp "
                     "(from {}) -> dragging to camp", bot->GetName(),
                     bot->GetExactDist(&camp),
                     phase == DcPullPhase::Forming ? "forming"
                         : phase == DcPullPhase::Idle ? "scouting" : "tag");
    }

    // CC-assist. If the tank is crowd-controlled mid-drag (stunned / feared /
    // confused / rooted, or slowed below PullCcSlowFloor) the retreat is failing —
    // it can't reach camp and just eats the pack while the party sits passive. Once
    // the CC has persisted past the grace, ABORT the pull: the tank STOPS the
    // run-home (cancelling the in-flight glide so it doesn't resume to camp when
    // the CC clears) and engages the pack where it stands, and the phase flips to
    // Engage — which both drops the party's passive camp-hold and trips
    // IsLeaderCampFightActive so the followers pile onto the pack and help. The
    // grace ignores a brief micro-CC so a single stutter-stun doesn't throw an
    // otherwise-fine pull away.
    if (DcSettings::GetBool(bot, "PullCcAssist"))
    {
        float const slowFloor = DcSettings::GetFloat(bot, "PullCcSlowFloor");
        char const* const ccReason = DcDragImpairReason(bot, slowFloor);
        uint32 const graceMs =
            uint32(DcSettings::GetFloat(bot, "PullCcAssistGrace") * 1000.0f);
        uint32 ccSinceOut = pull.ccSince;
        bool const ccAbort = DungeonClearMath::ShouldAbortPullForCc(
            ccReason != nullptr, pull.ccSince, now, graceMs, ccSinceOut);
        pull.ccSince = ccSinceOut;
        if (ccAbort)
        {
            pull.ccSince = 0;

            // Hard-cancel the run-home. The drag-back is a launched MoveTo
            // (MOVEMENT_COMBAT) glide; a plain StopMoving can leave it queued
            // UNDER the active CC generator, so the instant the impairment
            // wears off the tank resumes sprinting to the (now abandoned) camp
            // instead of fighting. StopBot(HardPin) drops any escort spline +
            // clears the LastMovement wait and pins the point-move on the spot
            // (unconditionally, since the bot may not be "moving" under CC).
            DcMovement::StopBot(bot, DcMovement::Stop::HardPin);

            DcSetPullPhase(context, DcPullPhase::Engage);

            // Start combat right here. Flipping to Engage already releases the
            // party (ReapStrandedPassives + IsLeaderCampFightActive), but the
            // tank itself must commit to the pack too: face and attack the
            // nearest attacker so it turns on the mob the moment the CC clears,
            // rather than drifting back toward camp before stock combat
            // re-acquires. Mirrors EngageDirect's in-range branch.
            Unit* aggressor = bot->GetVictim();
            for (Unit* a : bot->getAttackers())
            {
                if (!a || !a->IsAlive())
                    continue;
                if (!aggressor ||
                    bot->GetExactDist2d(a) < bot->GetExactDist2d(aggressor))
                    aggressor = a;
            }
            if (aggressor)
            {
                bot->SetSelection(aggressor->GetGUID());
                if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, aggressor))
                    ServerFacade::instance().SetFacingTo(bot, aggressor);
                context->GetValue<Unit*>(DcKey::Stock::CurrentTarget)->Set(aggressor);
                bot->Attack(aggressor, botAI->IsMelee(bot));
            }
            botAI->ChangeEngine(BOT_STATE_COMBAT);

            DC_PULL_INFO("[DC:{}] advanced-pull: tank {} mid-drag (held >{} ms) -> "
                         "abort pull, stop run-home + engage {}, releasing party "
                         "to assist", bot->GetName(), ccReason, graceMs,
                         aggressor ? aggressor->GetGUID().ToString() : "pack");
            return false;
        }
    }

    uint32 const since = pull.phaseSince;
    // `since` is stamped by DcSetPullPhase via its OWN getMSTime() call, which can
    // read a millisecond LATER than the `now` captured at the top of Execute. So on
    // the very tick we transition into Returning (above), now < since and a raw
    // `now - since` underflows to ~4.29e9 — instantly tripping the leg-timeout and
    // dumping the tank into "fight in place", which is why the pull-back to camp
    // worked or failed at random (a millisecond-boundary race). Clamp the elapsed.
    uint32 const legElapsed = now > since ? now - since : 0u;
    float const dist = bot->GetExactDist(&camp);

    if (dist <= DC_PULL_CAMP_ARRIVE)
    {
        // Back at camp. For an ordinary trash pull the pack is glued to the tank by
        // now (it chased it the whole way), so arriving IS the end of the maneuver:
        // stop, flip to Engage, and that flip is what releases the party.
        //
        // A PULL-BACK is different and releasing on tank-arrival alone is what made
        // the whole party beeline into the lake. The tank can be home long before
        // the boss is: it retreats from the water's edge while he is still swimming
        // in, and on a forced pull it may barely have moved at all. Flipping to
        // Engage there drops the camp hold, hands every follower to stock combat,
        // and stock combat's only visible target is a boss 100+yd away in the water
        // — so they all run at him.
        //
        // So hold the release until HE is actually here. Until then the tank waits
        // on the anchor and the party stays pinned and passive behind it, which is
        // the entire point of dragging him out. The leg watchdog below still bounds
        // it: a boss that never arrives falls out to "fight in place" rather than
        // holding the run forever.
        if (pull.bossPullback)
        {
            Unit* const pulled = pull.pullTarget.IsEmpty()
                ? nullptr : ObjectAccessor::GetUnit(*bot, pull.pullTarget);
            if (pulled && pulled->IsAlive() &&
                pulled->GetExactDist(&camp) > DC_PULLBACK_RELEASE_RANGE)
            {
                DcMovement::StopBot(bot, DcMovement::Stop::Soft);
                DcFaceIfNeeded(bot, pulled);

                // SUMMON-IF-STUCK. The tank is home and the boss is engaged and
                // inbound — but if he is still IN THE WATER BELOW the anchor, he is
                // not inbound in any useful sense: the way out of the lake is the
                // pipe, the pipe is absent from the extracted navmesh, and a chase
                // path with no geometry to follow leaves him hanging at the water's
                // edge indefinitely. There is nothing to wait for, so bring him up.
                //
                // The water-and-below test is what keeps this to the actual failure
                // rather than making it a general "boss is slow" shortcut: a boss
                // who has climbed out onto dry ground fails it and walks the rest of
                // the way himself, exactly as he should. Opt-in per row on top of
                // that (summonWhenStuckBelow) — relocating a boss is a big hammer.
                BossPullback const* const row =
                    BossPullbackRegistry::Find(bot->GetMapId(), pulled->GetEntry());
                if (row && row->summonWhenStuckBelow && pulled->IsInWater() &&
                    pulled->GetPositionZ() < camp.GetPositionZ())
                {
                    // Land him a few yards off the anchor on the tank's side rather
                    // than on top of the party: dropping a boss inside the healer is
                    // how a knockback or a cleave catches everyone at once. Snapped
                    // to the navmesh so he arrives on real ground; the anchor itself
                    // is the fallback (it is known-good — the party is standing on
                    // it).
                    float const bearing = camp.GetAngle(bot);
                    float const lx = camp.GetPositionX() + DC_PULLBACK_SUMMON_OFFSET *
                                                               std::cos(bearing);
                    float const ly = camp.GetPositionY() + DC_PULLBACK_SUMMON_OFFSET *
                                                               std::sin(bearing);
                    float lz = camp.GetPositionZ();
                    NavmeshSnap::Result const snap =
                        NavmeshSnap::Snap(bot->GetMap(), lx, ly, lz, 8.0f);
                    float const tx = snap.ok ? snap.x : camp.GetPositionX();
                    float const ty = snap.ok ? snap.y : camp.GetPositionY();
                    float const tz = snap.ok ? snap.z : camp.GetPositionZ();

                    LOG_INFO("playerbots.dungeonclear",
                             "[DC:{}] pull-back: {} is stuck in the water {:.1f}yd "
                             "below the anchor ({:.1f} vs {:.1f}) with no navmesh "
                             "route out — summoning him to ({:.1f},{:.1f},{:.1f})",
                             bot->GetName(), pulled->GetName(),
                             camp.GetPositionZ() - pulled->GetPositionZ(),
                             pulled->GetPositionZ(), camp.GetPositionZ(), tx, ty, tz);

                    pulled->NearTeleportTo(tx, ty, tz, pulled->GetAngle(bot));
                    return true;
                }

                DC_PULL_TRACE("[DC:{}] pull-back: home at the anchor, holding the "
                              "party — boss still {:.1f}yd out (release at {:.0f})",
                              bot->GetName(), pulled->GetExactDist(&camp),
                              DC_PULLBACK_RELEASE_RANGE);
                return true;
            }
        }

        DcMovement::StopBot(bot, DcMovement::Stop::Soft);
        DcSetPullPhase(context, DcPullPhase::Engage);
        DC_PULL_INFO("[DC:{}] advanced-pull: at camp ({:.1f}yd) -> engaging, party "
                     "released", bot->GetName(), dist);
        return false;
    }

    // Return-leg budget off the leg length stamped at the turn-around, so a
    // pull-back's long haul home isn't cut short (see DcPullLegTimeoutMs).
    //
    // For a pull-back the leg covers TWO journeys, not one, and budgeting only the
    // tank's would have made the watchdog fire during a perfectly healthy pull: the
    // tank runs ~60yd home in ~13s and then WAITS at the anchor for the boss, who
    // still has ~150yd of swim-and-ramp to cover. That wait happens inside this
    // leg. Add the boss's remaining distance so the budget covers both; it stays
    // bounded, so a boss who genuinely cannot path still falls out to fight-in-place
    // instead of holding the run open forever.
    float legBudgetDist = pull.returnLegStartDist;
    if (pull.bossPullback)
    {
        if (Unit* const pulled = pull.pullTarget.IsEmpty()
                ? nullptr : ObjectAccessor::GetUnit(*bot, pull.pullTarget))
            legBudgetDist += pulled->GetExactDist(&camp);
    }
    if (legElapsed > DcPullLegTimeoutMs(pull, legBudgetDist))
    {
        // Return leg wedged — fight where we are rather than freeze.
        DcSetPullPhase(context, DcPullPhase::Engage);
        DC_PULL_INFO("[DC:{}] advanced-pull: return leg wedged at {:.1f}yd from camp "
                     "after {} ms -> fighting in place",
                     bot->GetName(), dist, legElapsed);
        return false;
    }

    // Turn-and-plant. A human tank doesn't sprint the WHOLE leg back-turned; once
    // the pack is glued to it and chasing, it stops a few steps in, turns, and
    // fights — the fight happens wherever it actually plants. Stopping early is the
    // single biggest cut to the back-exposure window the daze-immunity cheat exists
    // to paper over. Suppressed for LOS-break pulls (the whole point is reaching
    // the corner) and gated on half the leg covered + a 2-tick debounce so a single
    // noisy distance read can't trip it. The plant point IS the new camp — re-stamp
    // it so the spread gate, status panel, and follower hold all follow the fight.
    // Suppressed outright for a PULL-BACK drag. Turn-and-plant makes the fight
    // happen "wherever the tank plants", which is exactly the freedom a pull-back
    // must not have: the anchor was chosen because it is the only safe ground, and
    // planting halfway home would drop the party back into the water the maneuver
    // exists to leave. Same reasoning as the losPull suppression inside
    // ShouldPlantEarly (there the point is reaching the corner; here it is reaching
    // the anchor) — a distinct flag rather than reusing losPull so the addon status
    // line and the LOS-camp logic keep their own meaning.
    if (DcSettings::GetBool(bot, "PullPlantEnable") && !pull.bossPullback)
    {
        float const glueRadius = DcSettings::GetFloat(bot, "PullPlantGlueRadius");
        std::vector<float> attackerDists;
        for (Unit* a : bot->getAttackers())
            if (a && a->IsAlive())
                attackerDists.push_back(bot->GetExactDist(a));

        if (DungeonClearMath::ShouldPlantEarly(attackerDists, glueRadius,
                /*glueTicksNeeded*/ 2u, pull.losPull, dist,
                pull.returnLegStartDist, pull.plantTicks))
        {
            // Hard-cancel the run-home exactly as the CC-abort branch does: a plain
            // StopMoving can leave the launched MOVEMENT_COMBAT glide queued and the
            // tank resumes sprinting to the abandoned camp instead of fighting.
            DcMovement::StopBot(bot, DcMovement::Stop::HardPin);

            // The camp IS wherever the fight lands. Re-stamp it (fresh publish) so
            // the party converges on the plant point and the addon panel relocates.
            pull.PublishCamp(Position(bot->GetPositionX(), bot->GetPositionY(),
                                      bot->GetPositionZ()),
                             now);

            // Turn on the nearest attacker so the tank commits the moment it stops,
            // rather than drifting before stock combat re-acquires (mirrors the
            // CC-abort engage block).
            Unit* nearest = bot->GetVictim();
            for (Unit* a : bot->getAttackers())
            {
                if (!a || !a->IsAlive())
                    continue;
                if (!nearest ||
                    bot->GetExactDist2d(a) < bot->GetExactDist2d(nearest))
                    nearest = a;
            }
            if (nearest)
            {
                bot->SetSelection(nearest->GetGUID());
                if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, nearest))
                    ServerFacade::instance().SetFacingTo(bot, nearest);
                context->GetValue<Unit*>(DcKey::Stock::CurrentTarget)->Set(nearest);
                bot->Attack(nearest, botAI->IsMelee(bot));
            }

            DcSetPullPhase(context, DcPullPhase::Engage);
            botAI->ChangeEngine(BOT_STATE_COMBAT);

            DC_PULL_INFO("[DC:{}] advanced-pull: pack gathered at {:.1f}yd from camp "
                         "-> plant + engage", bot->GetName(), dist);
            return false;
        }
    }

    // Run to camp. Own the tick (return true even on a duplicate move) so stock
    // combat chase/attack can't grab the tank and fight at the pack instead.
    DC_PULL_TRACE("[DC:{}] pull returning: {:.1f}yd to camp ({} ms into leg)",
                  bot->GetName(), dist, legElapsed);
    DcMoveTo(bot->GetMapId(), camp.GetPositionX(), camp.GetPositionY(), camp.GetPositionZ(),
           /*idle*/ false, /*react*/ false, /*normal_only*/ false,
           /*exact_waypoint*/ false, MovementPriority::MOVEMENT_COMBAT);
    return true;
}

