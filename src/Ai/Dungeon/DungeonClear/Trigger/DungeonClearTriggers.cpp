/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DungeonClearTriggers.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"

#include <cmath>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "CombatManager.h"
#include "Creature.h"
#include "Group.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Ai/Dungeon/DungeonClear/DcApproachState.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/FightInPlaceRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/RoomAggroRegistry.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DcEngageGeometry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcHazard.h"
#include "Ai/Dungeon/DungeonClear/Data/DcHazardRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRegroupDecision.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRezRecovery.h"
#include "Ai/Dungeon/DungeonClear/Util/DcStrandedRecovery.h"
#include "Ai/Dungeon/DungeonClear/Util/DcSmartRest.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearMath.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTickMemo.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonEventExecutor.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearTuning.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearUtil.h"
#include "Playerbots.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"

namespace
{
    // Shared tuning constants (DC_ENGAGE_RANGE, DC_TRASH_CONE_*,
    // DC_USE_CORRIDOR_SCAN, DC_CORRIDOR_*, DC_PULL_START_RANGE)
    // now live in DungeonClearTuning.h so the
    // trigger ladder and the action layer cannot drift. See that header for the
    // unit/why annotations.

    // How close a follower must be to the tank to rest during a persistent event
    // (otherwise it regroups to the tank's garrison spot first). See
    // RestTargetIfActive.
    constexpr float DC_EVENT_REST_REGROUP_DIST = 12.0f;

    // Combat-regroup debounce: the contribution predicate must hold continuously
    // for this long before the (non-emergency) reconnect fires, so a one-tick LOS
    // flicker can't launch a follower. The trigger ticks at interval 1. See
    // DungeonClearRegroupCombatTrigger + DcRegroupDecision.
    constexpr uint32 DC_REGROUP_DEBOUNCE_MS = 2000;

    // DC must be enabled AND not paused for the driving ladder to fire. Pause
    // is a soft stop: `enabled` (and all boss progress) stays set, but every
    // trigger here goes inert so the tank holds exactly as it would under
    // `dc off`. See DcRunState (paused flag).
    //
    // The driving ladder also runs ONLY on the elected leader. In a raid several
    // tanks can have the flag set, but exactly one drives — the others follow it
    // like any other member (see DungeonClearFollowTankTrigger). Only the leader
    // ever reaches the leadership scan: followers don't set `enabled`, so they
    // short-circuit on the first check. See DcLeaderSignal::FindLeaderTank.
    bool IsEnabled(AiObjectContext* context, Player* bot)
    {
        if (!DcRun::Of(context).enabled || DcRun::Of(context).paused)
            return false;
        return DcLeaderSignal::IsDungeonClearLeader(bot);
    }

    // Trigger-side between-pulls gate. Thin wrapper over the shared
    // DcPartyState::IsBetweenPullsReady (one body for the trigger ladder and the
    // advance action, which had drifted as two copies). The trigger side also
    // requires no pending loot — never start a pull over a corpse — while the
    // action side deliberately does not (its Execute owns loot behind a
    // commit-timeout).
    bool IsBetweenPullsReady(Player* bot, AiObjectContext* context)
    {
        // Memoised within the tick: this strict gate is read by five triggers in
        // one tick and each does a full party walk. See DcTickMemo.
        return DcTickMemoAccess::BetweenPullsReady(bot, context, /*requireNoLoot*/ true);
    }
}

bool DungeonClearIdleTrigger::IsActive()
{
    if (!IsEnabled(context, bot))
        return false;
    if (!bot || bot->IsInCombat() || bot->isDead())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, DcKey::NextDungeonBoss);
    if (!next.has_value())
        return false;

    // Fires whenever DC is on and combat is over. The advance action itself
    // decides between walking, holding (rest/loot/catch-up), or yielding to
    // the higher-priority at-boss trigger when in engage range and ready.
    // Always claiming this engine slot keeps grind/new-rpg from stealing the
    // tank during the wait.
    return true;
}

bool DungeonClearAtBossTrigger::IsActive()
{
    if (!IsEnabled(context, bot))
        return false;
    if (!bot || bot->IsInCombat() || bot->isDead())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, DcKey::NextDungeonBoss);
    if (!next.has_value())
        return false;

    // Travel objectives are not combat targets — the at-objective trigger owns
    // arrival. Stand down so engage-boss never fires on a non-creature anchor.
    if (next->kind != DungeonAnchorKind::Boss)
        return false;

    // Close enough AND on the boss's own floor (not just 3D-near while passing
    // under an upper-floor boss). See IsAtBossEngage.
    if (!DcTickMemoAccess::AtBossEngage(bot, context, *next))
        return false;

    // Room-wide-aggro boss (RoomAggroRegistry): on engage it force-pulls the
    // whole room, so HOLD the boss pull while any room trash remains. The
    // room-clear driver (Off/Leeroy) or the pull pipeline (advanced/dynamic)
    // clears it first; the gate reopens the instant the room is clear (or the
    // RoomClearTimeout valve fires inside the value). Cheap cached read.
    if (RoomAggroRegistry::Find(bot->GetMapId(), next->entry) &&
        !AI_VALUE(GuidVector, DcKey::RoomTrashRemaining).empty())
        return false;

    // A closed door between us and the boss means it's BEYOND it and not actually
    // reachable yet — even when it's within straight-line engage range (a boss or
    // its pack right behind the door). Without this, IsAtBossEngage (a pure
    // distance+floor test, door-blind) fires the at-boss engage, which outranks
    // door-blocked (30 vs 22) and bee-lines the tank onto/through the door. Stand
    // down so door-blocked parks at its stand-off; re-evaluates the instant the
    // door opens. Checked fresh, not via the 500ms-cached blocking-door value,
    // which can still read empty the moment the boss first comes into range.
    Creature* const liveBoss = DcTargeting::GetLiveBoss(bot, context, next->entry);
    float const bx = liveBoss ? liveBoss->GetPositionX() : next->x;
    float const by = liveBoss ? liveBoss->GetPositionY() : next->y;
    float const bz = liveBoss ? liveBoss->GetPositionZ() : next->z;
    if (DcEngageGeometry::ClosedDoorBetween(bot, bx, by, bz))
        return false;

    // When the long-path cache is anchored (registered route), make sure
    // all intermediate anchors have been resolved before firing. This
    // prevents the bot from "engaging" a boss it's geometrically near but
    // separated from by a wall or door — the cached anchor list runs the
    // bot around through the actual corridor first.
    ChunkedPathfinder::Result const& path =
        AI_VALUE(ChunkedPathfinder::Result&, DcKey::LongPath);
    if (path.reachable && !path.segments.empty())
    {
        bool anyAnchored = false;
        for (PathSegment const& seg : path.segments)
            if (seg.anchored) { anyAnchored = true; break; }
        if (anyAnchored)
        {
            // Last segment is the boss anchor; everything before it must
            // be within its arriveRadius.
            for (size_t i = 0; i + 1 < path.segments.size(); ++i)
            {
                PathSegment const& seg = path.segments[i];
                if (!seg.anchored)
                    continue;
                float const d = bot->GetDistance(seg.ex, seg.ey, seg.ez);
                if (d > seg.arriveRadius)
                    return false;
            }
        }
    }

    // Don't pull while party is still recovering or tank-side loot is pending.
    // The idle-trigger's advance action holds the tank in place meanwhile.
    return IsBetweenPullsReady(bot, context);
}

bool DungeonClearAtObjectiveTrigger::IsActive()
{
    if (!IsEnabled(context, bot))
        return false;
    if (!bot || bot->isDead())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, DcKey::NextDungeonBoss);
    if (!next.has_value() || next->kind != DungeonAnchorKind::Objective)
        return false;

    // Sticky for a PERSISTENT event already in progress: once started, stay live
    // regardless of distance so the tank can roam far from the anchor while the
    // event drives it (down ZulFarrak's stairs to the temple bosses, back to the
    // NPCs). Initial arrival still goes through the distance/gate check below;
    // completion latches the objective, after which `next` becomes the boss and
    // this returns false at the kind check above.
    if (DungeonEventExecutor::IsPersistentAnchoredEventActive(context))
        return true;

    // Satisfied when the tank has reached the anchor (within arriveRadius), or
    // when the optional gate creature has spawned alive (the event already fired
    // its result, e.g. the real boss is up), so we don't need to babysit it.
    float const radius = next->arriveRadius > 0.0f
                             ? next->arriveRadius
                             : DcSettings::GetFloat(bot, "ObjectiveArriveRadius");
    float const distToAnchor = bot->GetExactDist(next->x, next->y, next->z);
    if (distToAnchor <= radius)
        return true;

    // Diagnostic (throttled, leader only): when the tank is hovering NEAR an
    // objective but not arriving, this names the exact gap — dist vs arriveRadius
    // — so a "parks just short, never triggers the event" stall is unambiguous in
    // the log instead of inferred from spline distances.
    if (distToAnchor <= radius + 20.0f && DcLeaderSignal::IsDungeonClearLeader(bot))
    {
        // Per-leader throttle held in the leader's own DcApproachState — not a
        // function-local static, which would be shared across all leaders on all
        // MapUpdate.Threads (a data race AND cross-instance suppression, where one
        // instance's leader silences another's diagnostic).
        DcApproachState& appr =
            context->GetValue<DcApproachState&>(DcKey::ApproachState)->Get();
        uint32 const nowMs = getMSTime();
        if (getMSTimeDiff(appr.lastObjectiveDiagMs, nowMs) >= 3000)
        {
            appr.lastObjectiveDiagMs = nowMs;
            LOG_DEBUG("playerbots.dungeonclear",
                      "[DC:{}] objective '{}': dist={:.1f} > arriveRadius={:.1f} "
                      "(NOT arrived; event not started)",
                      bot->GetName(), next->name, distToAnchor, radius);
        }
    }

    if (next->gateEntry)
    {
        for (auto const& kv : map->GetCreatureBySpawnIdStore())
        {
            Creature* c = kv.second;
            if (c && c->GetEntry() == next->gateEntry && c->IsAlive())
                return true;
        }
    }
    return false;
}

bool DungeonClearEventDueTrigger::IsActive()
{
    if (!IsEnabled(context, bot))
        return false;
    if (!bot || bot->IsInCombat() || bot->isDead())
        return false;
    // Leader drives events; followers stay on follow-tank.
    if (!DcLeaderSignal::IsDungeonClearLeader(bot))
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;
    if (!DungeonEventRegistry::HasEvents(map->GetId()))
        return false;

    // Hand the trigger and the action the SAME "which event is due" answer so
    // they can never disagree about whether to fire / what to drive.
    return DungeonEventExecutor::FindDueConditionalEvent(bot, context, map->GetId()) != nullptr;
}

bool DungeonClearEventDueCombatTrigger::IsActive()
{
    if (!IsEnabled(context, bot))
        return false;
    // The mirror image of the non-combat gate: this copy exists FOR the in-combat
    // case, so a bot out of combat is the other rung's business.
    if (!bot || !bot->IsInCombat() || bot->isDead())
        return false;
    if (!DcLeaderSignal::IsDungeonClearLeader(bot))
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;
    if (!DungeonEventRegistry::HasEvents(map->GetId()))
        return false;

    // requireDrivesInCombat: only an opted-in wave event may take a tick from the
    // stock combat engine. Same call the action makes, so the two agree.
    return DungeonEventExecutor::FindDueConditionalEvent(bot, context, map->GetId(),
                                                         /*requireDrivesInCombat*/ true) != nullptr;
}

bool DungeonClearBlockingTrashTrigger::IsActive()
{
    if (!IsEnabled(context, bot))
        return false;
    if (!bot || bot->IsInCombat() || bot->isDead())
        return false;

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, DcKey::NextDungeonBoss);
    if (!next.has_value())
        return false;

    // At the boss (close AND on its floor), the at-boss trigger handles the
    // pull — don't also scan for blocking trash. While merely passing under an
    // upper-floor boss this is false, so trash on the way to the ramp still gets
    // cleared. See IsAtBossEngage.
    if (DcTickMemoAccess::AtBossEngage(bot, context, *next))
        return false;

    // Wait between pulls for loot, party catch-up, and rest.
    if (!IsBetweenPullsReady(bot, context))
        return false;

    // Prefer the wider DC-gated scan — packs at the far end of long
    // dungeon corridors fall outside the default 100yd sightDistance cap
    // that drives `possible targets`. Falls back to `possible targets`
    // when far-targets is empty (first tick, before its 500ms poll has
    // run).
    GuidVector const& farTargets = AI_VALUE(GuidVector, DcKey::FarTargets);
    GuidVector const& possibleTargets = AI_VALUE(GuidVector, DcKey::Stock::PossibleTargets);
    GuidVector const& candidates = farTargets.empty() ? possibleTargets : farTargets;

    Unit* trash = nullptr;
    if (DC_USE_CORRIDOR_SCAN)
    {
        // Walk the cached long-path polyline. The polyline spans the
        // entire chunked route — anchored or anchor-free — so blocking
        // trash beyond a single PathGenerator call is still detected.
        ChunkedPathfinder::Result const& path =
            AI_VALUE(ChunkedPathfinder::Result&, DcKey::LongPath);
        if (path.reachable && !path.segments.empty())
        {
            trash = DcTargeting::FindBlockingTrashOnPath(
                bot, path.segments, DC_CORRIDOR_LOOKAHEAD, DC_CORRIDOR_WIDTH, candidates);
        }
        // No usable long-path cache — fall back to a single-shot corridor
        // computed inline so the trigger stays live in degraded conditions.
        else
        {
            Movement::PointsArray corridor;
            if (DcEngageGeometry::ComputeCorridor(bot, next->x, next->y, next->z, corridor))
                trash = DcTargeting::FindBlockingTrashCorridor(
                    bot, corridor, DC_CORRIDOR_LOOKAHEAD, DC_CORRIDOR_WIDTH, candidates);
            else
                trash = DcTargeting::FindBlockingTrash(
                    bot, *next, DC_TRASH_CONE_RANGE, DC_TRASH_CONE_HALF_ANGLE, candidates);
        }
    }
    else
        trash = DcTargeting::FindBlockingTrash(
            bot, *next, DC_TRASH_CONE_RANGE, DC_TRASH_CONE_HALF_ANGLE, candidates);

    if (!trash)
        return false;

    // Don't engage a pack on the FAR side of a closed door. Some doors aren't
    // modeled as solid in the navmesh (the tank can clip through), so the scan
    // finds the far-side pack and engage-trash (priority 25) would otherwise
    // out-prioritise door-blocked (22) and run the tank through the door to it,
    // dragging the group into the fight. Checked FRESH (not via the 500ms-cached
    // blocking-door value), because the door often isn't flagged yet at the very
    // tick the scan first sees the pack — which let the tank run through, clear
    // it, and walk back. With the pack vetoed, door-blocked parks at the door;
    // this re-evaluates the instant the door opens.
    if (DcEngageGeometry::ClosedDoorBetween(bot, trash->GetPositionX(),
                                            trash->GetPositionY(), trash->GetPositionZ()))
    {
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] blocking-trash: vetoed pack {} ({:.1f}yd) — closed door "
                  "between us and it on our floor",
                  bot->GetName(), trash->GetGUID().ToString(), bot->GetExactDist(trash));
        return false;
    }

    // Patrol-wait hold (dynamic pull decision == 3): the pull pipeline is holding
    // the tank at commit range to let a patrol pass before committing the pull.
    // Stand down unconditionally so the tank doesn't walk in and engage mid-wait.
    if (AI_VALUE(DcPullContext&, DcKey::PullContext).decision == DcPullDecisionCode::PatrolHold)
    {
        DC_PULL_DEBUG("[DC:{}] blocking-trash: patrol-wait hold -> stand down",
                      bot->GetName());
        return false;
    }

    // In advanced-pull mode the pull pipeline OWNS the trash pack it is working:
    // it LOS-pulls it to camp rather than engaging in place. Stand down so the
    // tank glides in under Advance until the pull-start range, instead of
    // walking up and fighting here (engage-trash outranks advance, so without
    // this it would preempt the pull for any pack in the 20-35yd band the pull
    // is deliberately waiting to close). Two exceptions fall through to the
    // normal walk-in:
    //   * a pack a prior pull gave up on (abort target) — so the run never
    //     livelocks on it, and
    //   * a BYSTANDER — a pack the pull never selected — found by our
    //     aggro-shaped scan while the pull phase is IDLE (nothing in flight to
    //     disturb). Handing a pack the pipeline is not looking at to that
    //     pipeline was a silent no-op, and in heroic (always pull mode) it left
    //     Advance — the one component with no aggro awareness — as the only
    //     thing steering the tank into the room. Once anything is in flight
    //     (non-Idle) we keep standing down as before; the maneuver must not be
    //     thrashed. Decision core: DungeonClearMath::ShouldStandDownForPull.
    if (AI_VALUE(bool, DcKey::PullModeCurrent) &&
        trash->GetGUID() != AI_VALUE(DcPullContext&, DcKey::PullContext).abortTarget)
    {
        DcPullContext const& pull = AI_VALUE(DcPullContext&, DcKey::PullContext);
        bool const packIsPullsOwn = trash->GetGUID() == pull.decisionTarget ||
                                    trash->GetGUID() == pull.pullTarget;
        if (DungeonClearMath::ShouldStandDownForPull(
                packIsPullsOwn, pull.phase == DcPullPhase::Idle))
        {
            DC_PULL_DEBUG("[DC:{}] blocking-trash: pull mode owns pack {} ({:.1f}yd) -> "
                          "stand down for the pull pipeline",
                          bot->GetName(), trash->GetGUID().ToString(),
                          bot->GetExactDist(trash));
            return false;
        }
        DC_PULL_DEBUG("[DC:{}] blocking-trash: bystander pack {} ({:.1f}yd) with the "
                      "pull idle -> owning the tick",
                      bot->GetName(), trash->GetGUID().ToString(),
                      bot->GetExactDist(trash));
    }

    return true;
}

bool DungeonClearRoomTrashTrigger::IsActive()
{
    if (!IsEnabled(context, bot))
        return false;
    if (!bot || bot->IsInCombat() || bot->isDead())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    // Milestone 3: room-aggro is migrating onto the conditional-event path
    // (DungeonClearEventDueTrigger / DcRunEventAction at relevance 31). For a map
    // that already has a room-aggro pre-clear event authored, stand this legacy
    // rel-26 driver down so the two never both drive — the event path owns it.
    // Maps without an event row keep this path until they get one.
    if (DungeonEventRegistry::HasRoomAggroEvent(map->GetId()))
        return false;

    // Only at a flagged boss with room trash still up.
    if (!DcTargeting::IsRoomClearActive(bot, context))
        return false;

    // When pull-to-camp is in effect for this pack, the higher-priority pull
    // pipeline (relevance 35) owns the room clear so it honours the advanced/
    // dynamic pull type. This Leeroy room-clear is the Off / Dynamic-chose-Leeroy
    // path; stand down whenever the behavioural pull bool is set. The patrol-wait
    // hold (decision == 3) is pull-mode-off but likewise pull-pipeline-owned, so
    // stand down there too rather than Leeroy a room mob mid-wait.
    if (AI_VALUE(bool, DcKey::PullModeCurrent) ||
        AI_VALUE(DcPullContext&, DcKey::PullContext).decision == DcPullDecisionCode::PatrolHold)
        return false;

    // Same between-pulls gating the other engage triggers use (loot, party
    // catch-up, rest) so the room is cleared one careful pull at a time.
    if (!IsBetweenPullsReady(bot, context))
        return false;

    return DcTargeting::NearestRoomTrash(bot, context) != nullptr;
}

bool DungeonClearRoomPreClearHoldTrigger::IsActive()
{
    // IsEnabled already gates on enabled + not-paused + leader (tank). Followers
    // own themselves via follow-tank + their own skirt, so this is leader-only.
    if (!IsEnabled(context, bot))
        return false;
    if (!bot || bot->IsInCombat() || bot->isDead())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    // Necessary: the room-clear DRIVER window is open (flagged boss, trash up, bot
    // in the room envelope, on the boss's floor). The higher drivers (pull 35 /
    // event 31 / room-clear 26 / engage trash 25) outrank this, so when one is
    // actively pulling it runs and this never fires; this only ever takes the ticks
    // that would otherwise fall through to the room-aggro-blind Advance (15).
    if (!DcTargeting::IsRoomClearActive(bot, context))
        return false;

    // Sufficient: the bot is NEAR the boss — within the skirt orbit ring (avoid
    // sphere + party margin) plus a buffer. The DRIVER window spans the whole room
    // (so the clear can round the sphere and reach far packs); but the governor
    // only HOLDS, so it must engage ONLY near the boss. Out at a far pack or still
    // on the approach, holding would freeze the bot where the room-clear driver /
    // Advance must be free to move it — so we stand down there and let them drive,
    // and re-arm the moment the bot is back inside the keep-out ring. Near the boss
    // this is exactly the backstop that stops Advance creeping into the sphere
    // during a between-pulls gap. If Advance momentarily overshoots inward, this
    // re-asserts the hold the instant the bot is within the ring — still well
    // outside the boss's real wake distance.
    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, DcKey::NextDungeonBoss);
    if (!next.has_value())
        return false;
    Creature* const boss = DcTargeting::GetLiveBoss(bot, context, next->entry);
    if (!boss)
        return false;
    float const keepout = DcEngageGeometry::RoomAggroSphereRadius(bot, boss) +
                          DcSettings::GetFloat(bot, "RoomAggroPartyMargin") +
                          DC_ROOM_AGGRO_STANDOFF_BUFFER;
    return bot->GetDistance(boss) <= keepout;
}

bool DungeonClearPartyDiedTrigger::IsActive()
{
    if (!IsEnabled(context, bot))
        return false;
    if (!bot)
        return false;

    bool anyDead = bot->isDead();
    if (!anyDead)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot)
                continue;
            if (member->GetMapId() != bot->GetMapId())
                continue;
            if (member->isDead())
            {
                anyDead = true;
                break;
            }
        }
    }
    if (!anyDead)
        return false;

    // Post-combat rez: a death only fires the disable bailout when recovery is
    // NOT viable — full wipe, no living rez class, recovery timed out, or the
    // feature switched off (Evaluate returns Disable/Disabled then, preserving
    // the classic behavior). While viable, the verdict is Hold: this trigger
    // stays silent, the rez-party rung drives the recovery, and the between-
    // pulls / event-rest IsPending gates keep the run parked over the corpse.
    // Evaluate also maintains the recovery clock + announcements as its side
    // effect — this per-tick call is one of the clock's two update sites (the
    // rez trigger on every bot is the other, covering a dead leader).
    return DcRezRecovery::Evaluate(bot).verdict.outcome ==
           DcRezDecision::Outcome::Disable;
}

bool DungeonClearRezPartyTrigger::IsActive()
{
    // Runs on EVERY bot with the DC strategy — the elected rezzer may be a
    // follower, or the leader itself (a prot paladin raising its healer). No
    // IsEnabled leader gate: Evaluate resolves the run owner dead-tolerantly
    // (the leader may BE the corpse) and returns None for off/paused runs.
    if (!bot || bot->isDead() || bot->IsInCombat())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    DcRezRecovery::Plan const plan = DcRezRecovery::Evaluate(bot);
    return plan.verdict.outcome == DcRezDecision::Outcome::Hold &&
           plan.verdict.reason == DcRezDecision::Reason::Recovering &&
           plan.rezzer == bot->GetGUID();
}

bool DungeonClearAllClearedTrigger::IsActive()
{
    if (!IsEnabled(context, bot))
        return false;
    if (!bot)
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    auto const& bosses = AI_VALUE(std::vector<DungeonBossInfo>, DcKey::DungeonBosses);
    if (bosses.empty())
        return false;

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, DcKey::NextDungeonBoss);
    return !next.has_value();
}

bool DungeonClearRecoverStrandedTrigger::IsActive()
{
    if (!bot || bot->isDead())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    // Evaluate is the failsafe's clock owner: it ticks the leader's no-progress
    // clock as a side effect (leader-only inside; a no-op on followers and on
    // off/paused runs) and returns true only when a rescue teleport is due.
    return DcStrandedRecovery::Evaluate(bot);
}

bool DungeonClearStalledTrigger::IsActive()
{
    if (!IsEnabled(context, bot))
        return false;
    if (!bot || bot->IsInCombat() || bot->isDead())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    // Only fall back when there is an actual stall reason set by Advance or
    // EngageBoss. If the path is clear, this trigger never fires.
    std::string const& reason = AI_VALUE(std::string&, DcKey::StallReason);
    if (reason.empty())
        return false;

    // Wait between pulls for loot, party catch-up, and rest before pulling
    // anything new — same gating the trash/boss triggers use.
    return IsBetweenPullsReady(bot, context);
}

bool DungeonClearDoorBlockedTrigger::IsActive()
{
    if (!IsEnabled(context, bot))
        return false;
    if (!bot || bot->IsInCombat() || bot->isDead())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    // Stand down when a CONDITIONAL event is due for this map: the event owns the
    // way forward (e.g. Uldaman's keystone opens the Seal of Khaz'Mul, SFK's freed
    // prisoner opens the courtyard door), and DcRunEventAction (relevance 31) must
    // be the one to drive it. Parking + auto-pausing here would otherwise race
    // ahead of the event and DEADLOCK an event whose door the bot must ACTIVELY
    // open (a paused run can't drive the keystone). FindDueConditionalEvent reads
    // false again the moment the event latches / the door opens, so this only
    // yields for the narrow window the event is actually live. Same answer the
    // event-due trigger uses, so the two never disagree.
    if (DungeonEventRegistry::HasEvents(map->GetId()) &&
        DcLeaderSignal::IsDungeonClearLeader(bot) &&
        DungeonEventExecutor::FindDueConditionalEvent(bot, context, map->GetId()) != nullptr)
        return false;

    // Non-empty GUID means the blocking-door value found a closed door
    // within the corridor. Empty means clear path. Polled at 500ms by the
    // value itself, so this trigger is cheap.
    ObjectGuid const door = AI_VALUE(ObjectGuid, DcKey::BlockingDoor);
    return !door.IsEmpty();
}

bool DungeonClearDoorReopenedTrigger::IsActive()
{
    if (!bot || bot->isDead())
        return false;
    // Only meaningful while THIS bot's own run is paused for a door (the leader's
    // paused flag). A manual `dc pause` leaves the paused-door GUID empty, so
    // opening some unrelated door never auto-resumes a hand-held pause.
    if (!DcRun::Of(context).enabled || !DcRun::Of(context).paused)
        return false;

    ObjectGuid const doorGuid = DcRun::Of(context).pausedDoor;
    if (doorGuid.IsEmpty())
        return false;

    // Resume once the blocker is gone: the door now reads OPEN (a player opened
    // it), or it despawned / its grid unloaded (GetGameObject returns null). In
    // either case the corridor is no longer held — IsDoorClosed treats a null GO
    // as not-closed, so the single test covers both.
    GameObject* door = botAI->GetGameObject(doorGuid);
    return !DcEngageGeometry::IsDoorClosed(door);
}

bool DungeonClearFollowTankTrigger::IsActive()
{
    if (!bot || bot->isDead() || bot->IsInCombat())
        return false;

    // In advanced-pull mode the party HOLDS and leapfrogs camp-to-camp instead of
    // following the tank (see DungeonClearHoldAtCampTrigger) — following would
    // trail the tank forward into every pull, which is the "party piles onto the
    // pull" chaos. Stand down whenever a camp is established so hold-at-camp owns
    // the follower; it also tears down any leftover MoveFollow generator itself.
    {
        Position camp;
        bool passive = false;
        if (DcLeaderSignal::GetLeaderCampHold(bot, camp, passive))
            return false;
    }

    // Redirect every non-leader bot to the leader — non-tanks AND non-leader
    // (off-)tanks in a raid. The leader resolves "party tank" to itself, so the
    // `tank != bot` guard below excludes only the leader (it doesn't follow
    // itself); a non-leader tank follows the leader out of combat just like any
    // DPS, then peels off to help tank once it enters combat (checked above).
    Player* tank = AI_VALUE(Player*, DcKey::PartyTank);
    if (tank && tank != bot)
    {
        // No distance gate: while DC is active on the tank, the follow-tank
        // action runs every non-combat tick. MovementAction::Follow yields a
        // no-op when already in range, so this is cheap. Without it, the
        // follower's `move from group` (anti-collision) preempts the default
        // `follow` strategy at melee range and they drift backward each tick.
        return true;
    }

    // No DC tank anymore. Keep firing for the one teardown tick needed to
    // cancel the leftover continuous MoveFollow this bot installed while it
    // was following the tank — MoveFollow is a persistent MotionMaster order,
    // and a self-bot (master == itself) never replaces it via its ordinary
    // follow, so without this it stays glued to the tank after dc off. The
    // action clears that GUID once it tears the follow down, so this stops
    // firing immediately after.
    return !AI_VALUE(ObjectGuid, DcKey::FollowedTank).IsEmpty();
}

namespace
{
    // Shared gate for the rest-target triggers: this bot is a living, out-of-
    // combat member of an active DC run (the cross-bot party-tank value is
    // non-null only while the leader's clear runs and is unpaused), and the run
    // has set a non-zero rest target for the given key. Returns the target % (0
    // when the trigger should stay inert).
    uint32 RestTargetIfActive(Player* bot, AiObjectContext* context, char const* key)
    {
        if (!bot || bot->isDead() || bot->IsInCombat())
            return 0;
        Player* tank = AI_VALUE(Player*, DcKey::PartyTank);
        if (!tank)
            return 0;

        // During a PERSISTENT anchored event (ZulFarrak's temple), don't rest
        // until regrouped near the tank: a member that dropped low at the bottom
        // would otherwise drink ON the wave spawn and the next wave spawns on top
        // of it. Deferring (return 0) lets follow-tank (rel 25) win, so the member
        // first runs up to the tank's garrison spot on the ramp, THEN rests there.
        // The tank itself (tank == bot) reads distance 0 and rests at its garrison.
        if (tank != bot)
        {
            PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank);
            if (tankAI &&
                DungeonEventExecutor::IsPersistentAnchoredEventActive(
                    tankAI->GetAiObjectContext()) &&
                bot->GetExactDist(tank) > DC_EVENT_REST_REGROUP_DIST)
                return 0;
        }

        // Smart Rest: the rest target is not a per-key setting but the party
        // latch — while latched EVERYONE rests to full (target 100), released
        // the trigger goes inert (target 0; between-rests suppression is the
        // multiplier's job, so nothing fires against a 0-multiplier).
        if (DcSettings::GetBool(bot, "SmartRest"))
            return DcSmartRest::IsLatched(tank) ? 100 : 0;

        return DcSettings::GetUInt(bot, key);
    }
}

bool DungeonClearNeedsDrinkTrigger::IsActive()
{
    uint32 const target = RestTargetIfActive(bot, context, "RestManaPct");
    if (target == 0)
        return false;
    // Non-mana classes (warriors/rogues) never drink.
    if (bot->GetMaxPower(POWER_MANA) == 0)
        return false;
    return bot->GetPowerPct(POWER_MANA) < static_cast<float>(target);
}

bool DungeonClearNeedsEatTrigger::IsActive()
{
    uint32 const target = RestTargetIfActive(bot, context, "RestHealthPct");
    if (target == 0)
        return false;
    return bot->GetHealthPct() < static_cast<float>(target);
}

bool DungeonClearPullTrigger::IsActive()
{
    if (!IsEnabled(context, bot))  // enabled, not paused, leader
        return false;
    if (!bot || bot->isDead())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    // `dungeon clear pull mode current` refreshes the Dynamic (pull setting == 2)
    // Leeroy/Advanced verdict for THIS tick and returns the behavioural pull-mode
    // bool. Reading it here (instead of mutating the bool as a side effect of this
    // IsActive() and hoping later readers run after us) keeps the verdict
    // order-independent: every reader — the engage/blocking-trash triggers and the
    // camp gates — consults the same value, so whichever runs first updates it.
    // No-op for Off/On, where DcPullAction owns the bool.
    bool const pullModeCurrent = AI_VALUE(bool, DcKey::PullModeCurrent);
    // decision == 3 is the patrol-wait HOLD: pull mode is off-but-held, so the
    // behavioural bool reads false, but the pull action must still run to halt the
    // tank at commit range while it waits the patrol out. Keep the trigger live in
    // that state too. (Reading pull mode current FIRST runs the governor that may
    // set decision == 3 this tick.)
    bool const patrolWaiting =
        AI_VALUE(DcPullContext&, DcKey::PullContext).decision == DcPullDecisionCode::PatrolHold;
    // A PULL-BACK boss (BossPullbackRegistry) runs the maneuver REGARDLESS of the
    // player's pull setting. It isn't a tactical preference there: Ghaz'an's home
    // is open water over a 47yd pit, so "pull Off" would mean the walk-in engage
    // swims the party out to him — the wipe. The registry row IS the decision.
    // Once the maneuver is in flight `bossPullback` keeps this true across the
    // phases even if the tank drags out of the anchor's at-boss radius.
    bool const pullback =
        AI_VALUE(DcPullContext&, DcKey::PullContext).bossPullback ||
        DcTargeting::IsPullbackBossDue(bot, context);
    if (!pullModeCurrent && !patrolWaiting && !pullback)
        return false;

    uint32 const phase = static_cast<uint32>(AI_VALUE(DcPullContext&, DcKey::PullContext).phase);

    // Mid-pull pre-combat (Forming/Advancing) and the post-fight Engage cleanup
    // run on this non-combat engine, but only while out of combat — the instant
    // the tank aggros, control passes to the combat maneuver trigger.
    if (phase != static_cast<uint32>(DcPullPhase::Idle))
        return !bot->IsInCombat();

    // Idle: decide whether to START a pull. Must be out of combat, not at the
    // boss (trash-only), and the between-pulls gates clear (loot/rest/catch-up).
    if (bot->IsInCombat())
        return false;

    std::optional<DungeonBossInfo> next = AI_VALUE(std::optional<DungeonBossInfo>, DcKey::NextDungeonBoss);
    if (!next.has_value())
        return false;
    // Parked at a pending-summon boss (e.g. RFD's gong -> Tuten'kash): do NOT
    // start a pull. The tank must hold at the anchor to run the event (ring,
    // fight the wave, ring again); a dynamic pull here latches a distant trash
    // pack and tows the tank out of the room, and it never returns to finish the
    // rings. The at-boss stand-down below only covers ~engage range, which the
    // wave fight pushes the tank past — this holds the wider event radius.
    if (DcTargeting::IsHoldingForSummonEvent(bot, context, *next))
        return false;
    // Normally the pull pipeline stands down at the boss (the at-boss engage owns
    // it). There are two exceptions. A room-wide-aggro boss with room trash still
    // up: there the pull pipeline is what clears that room (honouring advanced/
    // dynamic pull), so it must stay live at the boss until the room is clear. And
    // a PULL-BACK boss: the pull pipeline is the ONLY thing that may engage him,
    // because the walk-in engage would go to where he stands — which for Ghaz'an is
    // the middle of a lake.
    if (DcTickMemoAccess::AtBossEngage(bot, context, *next) &&
        !DcTargeting::IsRoomClearActive(bot, context) && !pullback)
        return false;
    if (!IsBetweenPullsReady(bot, context))
        return false;

    Unit* trash = DcTargeting::GetPullTarget(botAI);
    if (!trash)
        return false;

    // Fight-in-place room: some bosses gate their OWN aggro on the party being inside
    // their room (Selin Fireheart's CanAIAttack requires X > 216). Advanced-pulling
    // his room-guards back to an antechamber camp drags the fight outside that gate,
    // so the boss aggros but can never attack — a permanent one-sided combat flag.
    // Never pull a mob that lives in such a room; defer to the walk-in engage, which
    // fights it in place and lands the party inside the boss's aggro gate.
    if (FightInPlaceRegistry::IsNoPullZone(bot->GetMapId(), trash->GetPositionX(),
                                           trash->GetPositionY()))
    {
        DC_PULL_DEBUG("[DC:{}] pull trigger: target {} is in a fight-in-place room -> "
                      "defer to walk-in engage", bot->GetName(), trash->GetGUID().ToString());
        return false;
    }

    // Don't loop on a pack a previous pull gave up on — let engage-trash walk in.
    if (AI_VALUE(DcPullContext&, DcKey::PullContext).abortTarget == trash->GetGUID())
    {
        DC_PULL_DEBUG("[DC:{}] pull trigger: target {} is the abort target -> defer "
                      "to normal engage", bot->GetName(), trash->GetGUID().ToString());
        return false;
    }

    // We fire even while the pack is still beyond run-in reach (the pull-target scan
    // already caps the look-ahead at ~35yd). The action does NOT commit yet — its
    // Idle branch yields to Advance to glide the tank closer — but running it now
    // lets it PUBLISH a prospective camp each glide tick so the party walks up to
    // the camp IN PARALLEL with the tank's approach instead of waiting for the
    // tank to arrive and only then trudging forward. The blocking-trash trigger
    // still stands down in pull mode, so Advance keeps driving the glide.
    float const toTrash = bot->GetExactDist2d(trash);
    float const commitRange =
        DcEngageGeometry::PullCommitRange(bot, trash, DC_PULL_START_RANGE);
    DC_PULL_DEBUG("[DC:{}] pull trigger: active — target {} at {:.1f}yd (commit {:.1f}, {})",
                  bot->GetName(), trash->GetGUID().ToString(), toTrash, commitRange,
                  toTrash > commitRange ? "glide + advance party camp" : "commit");
    return true;
}

bool DungeonClearPullManeuverTrigger::IsActive()
{
    if (!bot || bot->isDead() || !bot->IsInCombat())
        return false;
    // `paused` is deliberately NOT checked here: pause is a soft stop, and
    // abandoning the tank mid-drag with a live pack on it is worse than letting
    // the drag finish. The follower side agrees — GetLeaderPullInfo /
    // GetLeaderCampHold keep the camp hold alive through a pause while a
    // maneuver phase is holding — so the party stays pinned at camp until the
    // drag resolves to Engage, after which the normal paused gates hold the run.
    // `bossPullback` is an alternative to the pull-mode bool for the same reason
    // the non-combat pull trigger accepts it: a BossPullbackRegistry drag runs
    // whatever the player's pull setting is, and the drag-back leg is the whole
    // point of it. Without this the tag would land and then nothing would haul the
    // boss home — the tank would fight Ghaz'an where it tagged him, in the water.
    if (!DcRun::Of(context).enabled ||
        (!AI_VALUE(bool, DcKey::PullMode) &&
         !AI_VALUE(DcPullContext&, DcKey::PullContext).bossPullback))
        return false;
    if (!DcLeaderSignal::IsDungeonClearLeader(bot))
        return false;

    uint32 const phase = static_cast<uint32>(AI_VALUE(DcPullContext&, DcKey::PullContext).phase);
    // Forming is included so combat taken DURING the pre-run-in retreat/dwell is
    // not a dead zone: the non-combat pull trigger goes silent the instant the
    // tank is in combat (it returns !IsInCombat for any non-Idle phase), and if
    // the maneuver didn't pick Forming up here NOTHING would drive the pull —
    // the party would stay passive while the tank solos until the mob dies. With
    // Forming handled, an early aggro hands straight to the drag-back maneuver.
    //
    // Idle is included for the SAME reason one phase earlier: while the tank is
    // merely scouting toward the next pack (phase Idle, not yet committed) a
    // patrol/add can aggro it. Without this the maneuver wouldn't fire and stock
    // combat would fight in place wherever the tank happened to be — the "starts
    // combat right here instead of moving back to camp" bug. With Idle handled,
    // any unplanned aggro also drags back to the held party at camp first.
    // (Engage is deliberately still excluded so the camp fight itself runs
    // normally — by then the phase is Engage, never Idle.)
    return phase == static_cast<uint32>(DcPullPhase::Idle) ||
           phase == static_cast<uint32>(DcPullPhase::Forming) ||
           phase == static_cast<uint32>(DcPullPhase::Advancing) ||
           phase == static_cast<uint32>(DcPullPhase::Returning);
}

bool DungeonClearHoldAtCampTrigger::IsActive()
{
    if (!bot || bot->isDead() || bot->IsInCombat())
        return false;
    // The leader drives the pull; it never holds at its own camp.
    if (DcLeaderSignal::IsDungeonClearLeader(bot))
        return false;

    // Hold at camp throughout pull mode (NOT just mid-maneuver): in pull mode the
    // party leapfrogs camp-to-camp and never follows the tank, so this fires while
    // the tank is merely scouting between pulls too. Passive is applied by the
    // action only during the holding phases (see GetLeaderCampHold's passiveOut).
    Position camp;
    bool passive = false;
    return DcLeaderSignal::GetLeaderCampHold(bot, camp, passive);
}

bool DungeonClearHoldAtCampCombatTrigger::IsActive()
{
    // Combat-engine twin of DungeonClearHoldAtCampTrigger, but it fires ONLY
    // during the holding pull phases (passiveOut) — when the tank is tagging and
    // the party must stay pinned and passive across the combat boundary the pull
    // drags it over. During the camp fight (Engage) and any unplanned aggro while
    // scouting (Idle) it deliberately does NOT fire, so the party fights normally.
    if (!bot || bot->isDead() || !bot->IsInCombat())
        return false;
    // The leader drives the pull; it never holds at its own camp.
    if (DcLeaderSignal::IsDungeonClearLeader(bot))
        return false;

    Position camp;
    bool passive = false;
    if (!DcLeaderSignal::GetLeaderCampHold(bot, camp, passive))
        return false;
    return passive;
}

namespace
{
    // Gate for the COMBAT-side fight assist: this follower's leader is mid
    // fight AND the bot currently has NO line-of-sight target of its own. The
    // empty-attackers test is what makes the combat assist self-limiting: the
    // stock AttackersValue LOS-filters, so "attackers" is empty exactly while the
    // pack is out of sight (the bug) and becomes non-empty the moment movement
    // carries the bot back into sight — at which point this goes inert and the
    // bot's own combat rotation (NOT multiplier-suppressed in the combat engine)
    // owns the fight again. The NON-combat side deliberately does NOT use this gate
    // (see DungeonClearAssistCampTrigger).
    bool ShouldAssistCampFight(PlayerbotAI* botAI, Player* bot)
    {
        // Healers are owned by DungeonClearHealRepositionTrigger, which positions
        // them relative to their hurt heal target (LOS + heal range) instead of
        // driving them onto the pack. Letting assist also grab a healer made the
        // two fight over it and aimed the healer at the mob, not the tank.
        if (PlayerbotAI::IsHeal(bot))
            return false;
        if (!DcLeaderSignal::IsLeaderFightAssistWanted(bot))
            return false;

        // Fire unless the bot has an attacker it can ACT ON from where it stands —
        // i.e. one within melee/cast range AND line of sight. The old test was a
        // bare `attackers.empty()`, which parked the DPS the moment ANY mob was on
        // its (LOS-filtered) list — even one far out of reach that stock combat
        // never closed on, or a single mob flickering in/out of sight as the tank
        // repositioned. That is the observed freeze (live: myAttackers=1, moving=0,
        // not fighting) and the exact asymmetry the player called out: the healer's
        // reposition has no such gate, so it keeps orbiting the tank while the DPS
        // stall. Mirror the healer: assist (reposition to the tank's fight) whenever
        // nothing is engageable from here; stand down only when there is a real
        // target the rotation can already hit.
        GuidVector const& attackers =
            botAI->GetAiObjectContext()->GetValue<GuidVector>(DcKey::Stock::Attackers)->Get();
        if (attackers.empty())
            return true;
        float const range = botAI->IsMelee(bot)
            ? (bot->GetCombatReach() + 5.0f)
            : botAI->GetRange("spell");
        for (ObjectGuid const& guid : attackers)
        {
            Unit* u = ObjectAccessor::GetUnit(*bot, guid);
            if (u && u->IsAlive() && bot->GetExactDist(u) <= range &&
                bot->IsWithinLOSInMap(u))
                return false;  // an engageable target is in reach — let combat fight it
        }
        return true;  // nothing engageable from here — reposition to the fight
    }
}

bool DungeonClearAssistCampTrigger::IsActive()
{
    // Non-combat side: ANY follower still out of combat while the leader tank
    // fights must be driven in — NOT just the out-of-LOS ones. An idle follower
    // that DOES have line of sight to the fight cannot self-engage either: DC's
    // multiplier suppresses the stock proactive-engagement pickers ("attack
    // anything" / pull) for every follower while a clear is active, so without an
    // explicit push it just stands there watching. And a follower the fight never
    // touched (tank aggroed around a corner / past natural engage range) never
    // enters combat at all, so the combat-engine regroup/assist can't reach it.
    // The assist action force-targets the pack and SetInCombatWith()s the bot,
    // flipping it into the combat engine where its own rotation/heal logic
    // (un-suppressed there) takes over. Hence we gate on the leader's fight
    // alone, not on an empty attacker list. Covers the advanced-pull camp fight
    // AND every fight the camp machinery does not own (Leeroy/dynamic/boss);
    // defers to the camp hold during the passive pull phases.
    //
    // NOT gated on !bot->IsInCombat() — that was the freeze. When the tank pulls, the
    // followers get GROUP-flagged in combat, but with no attacker/target of their own
    // (the pack is on the tank, out of their sight) the playerbots AI keeps them in
    // the NON-COMBAT engine. In that limbo the old IsInCombat gate stood this trigger
    // AND follow-tank down, while the combat-side assist never ran (wrong engine) —
    // so nothing drove them and they froze around the corner (proven live: scout-lag
    // running with selfCombat=1). This trigger only ever evaluates in the non-combat
    // engine anyway, so a follower reaching here is not actively fighting; drive it to
    // the tank's fight regardless of the combat flag. The action flips it into the
    // combat engine once it reaches LOS/range.
    if (!bot || bot->isDead())
        return false;
    // Healers are owned by the heal-reposition governor (aims at the hurt heal
    // target, not the pack); keep assist for DPS that must be driven into the fight.
    if (PlayerbotAI::IsHeal(bot))
        return false;
    return DcLeaderSignal::IsLeaderFightAssistWanted(bot);
}

bool DungeonClearAssistCampCombatTrigger::IsActive()
{
    // Combat side: a follower dragged into combat (group combat / stray hit) but
    // with the pack around a corner has an empty LOS attacker list and so idles
    // in the combat engine. Drive it into sight so stock combat can engage.
    if (!bot || bot->isDead() || !bot->IsInCombat())
        return false;
    return ShouldAssistCampFight(botAI, bot);
}

bool DungeonClearLeaderAssistTrigger::IsActive()
{
    // Leader-side assist. All the gating (leader-only, out of combat, no own
    // target, a groupmate latched in combat, not mid-drag) lives in the predicate.
    return DcLeaderSignal::IsLeaderShouldAssistFight(bot);
}

bool DungeonClearObjectiveEngageCombatTrigger::IsActive()
{
    // IsEnabled also gates leader-only + live/unpaused run. Combat engine only.
    if (!IsEnabled(context, bot))
        return false;
    if (!bot || bot->isDead() || !bot->IsInCombat())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;

    // While the active objective's event HAS a KillCreature-ENGAGE step. anyStep:
    // arm even during the leading MoveTo (step 0), so a sapper that flags us into
    // combat before the tank reaches the anchor (and the non-combat driver advances
    // to the engage step) still gets its stealth broken — that pre-arrival window
    // is the exact deadlock this rung exists to break.
    uint32 entry = 0;
    float search = 0.0f;
    if (!DungeonEventExecutor::ActiveEngageStep(context, entry, search, /*anyStep*/ true))
        return false;

    // Deadlock signature: a live creature of the step's entry sits nearby and is
    // REACHABLE, but this bot canNOT see/detect it — a stealthed sapper that flagged
    // us into combat and re-stealthed, leaving stock combat with no victim. While it
    // stays undetectable, drive EngageDirect by entry (the action) to break stealth.
    // Once detectable, this returns false and stock combat owns the kill. Using the
    // nearest instance as the probe is sufficient: if the nearest is detectable the
    // party is NOT deadlocked (stock combat has a target and makes progress), and a
    // farther stealthed one becomes the nearest once that kill completes.
    Creature* c = bot->FindNearestCreature(entry, search, /*alive*/ true);
    if (!c)
        return false;
    if (bot->CanSeeOrDetect(c))
        return false;  // detectable -> stock combat handles it, this stands down
    return DcEngageGeometry::IsReachable(bot, c->GetPositionX(), c->GetPositionY(),
                                         c->GetPositionZ());
}

namespace
{
    // Is the bot's combat explained by something we must RESPECT (so it is NOT a
    // phantom flag we may clear)? Walk the bot's own CombatManager PvE references
    // (each carries a guaranteed-valid other unit) and answer true if EITHER:
    //   * there are no unit references at all — an opaque, script-forced combat with
    //     no enemy to blame; we never touch that, the core/script owns it; OR
    //   * at least one holder is still RESOLVABLE: alive, on the bot's map, NOT
    //     evading (an evading mob is leaving combat), and PATH-REACHABLE from the bot.
    //
    // Keying on reachability — not distance — is the safety property: a pursuer in a
    // flee or kite is always path-reachable (that is how it chases and how the party
    // fled from it), so it always counts as resolvable and the escape hatch stays
    // inert. Only a holder the bot genuinely cannot path to (spawned behind a closed
    // gate / across a navmesh gap) fails the test — exactly the ghost-flag case.
    bool HasLegitimateCombatHolder(Player* bot)
    {
        auto const& refs = bot->GetCombatManager().GetPvECombatRefs();
        if (refs.empty())
            return true;  // no unit to blame -> opaque/forced combat, leave it alone

        Map* const map = bot->GetMap();
        for (auto const& kv : refs)
        {
            CombatReference* const ref = kv.second;
            if (!ref)
                continue;
            Unit* const other = ref->GetOther(bot);
            if (!other || !other->IsAlive() || other->GetMap() != map)
                continue;
            if (other->GetCombatManager().IsInEvadeMode())
                continue;  // holder is bailing home -> not a real threat
            if (!DcEngageGeometry::IsReachable(bot, other->GetPositionX(),
                                               other->GetPositionY(), other->GetPositionZ()))
                continue;  // unreachable -> the phantom holder
            return true;   // a reachable, live, non-evading holder: a REAL fight
        }
        return false;
    }
}

bool DungeonClearBreakStuckCombatTrigger::IsActive()
{
    // Only meaningful while flagged in combat. Out of combat -> nothing to break;
    // reset the streak so a fresh stall later starts its clock clean.
    if (!bot || bot->isDead() || !bot->IsInCombat())
    {
        stuckCombatSinceMs = 0;
        return false;
    }

    // Never force-clear combat in a RAID zone. A raid encounter is exactly where an
    // errant combat drop does the most damage (a wrongly-cleared boss reference can
    // reset the fight for the whole raid), the phantom-combat deadlock this recovers
    // is a 5-man dungeon-clear problem, and raid bosses routinely hold the raid in
    // combat with no per-bot reachable target during phase transitions / adds — the
    // precise shape this would misread. Gate it out entirely rather than trust the
    // reachability test there.
    Map* const map = bot->GetMap();
    if (!map || map->IsRaid())
    {
        stuckCombatSinceMs = 0;
        return false;
    }

    // Confine the force-clear to a live, UNPAUSED DC run this bot participates in.
    // PartyTank resolves to the elected leader (or the leader itself); null means DC
    // is off, paused, or this bot isn't in a DC party — in all three we leave the
    // core combat state alone (a paused run is a deliberate hand-off to the player).
    if (!AI_VALUE(Player*, DcKey::PartyTank))
    {
        stuckCombatSinceMs = 0;
        return false;
    }

    // Phantom signature: nothing meleeing us, no victim of our own, and every unit
    // holding us in combat is unreachable/evading (or — the opaque case — there is
    // none to blame, which HasLegitimateCombatHolder reports as legitimate). Short-
    // circuit the path-querying holder scan behind the two cheap reads: a real fight
    // almost always trips hasAttacker/hasVictim, and a fleeing/kiting party trips the
    // reachable-holder test — either way we never run down the timer.
    bool const hasAttacker = !bot->getAttackers().empty();
    bool const hasVictim   = bot->GetVictim() != nullptr;
    bool const hasHolder =
        hasAttacker || hasVictim || HasLegitimateCombatHolder(bot);

    bool const phantom =
        DungeonClearMath::IsPhantomCombat(/*inCombat*/ true, hasAttacker, hasVictim, hasHolder);

    uint32 const timeoutMs =
        uint32(DcSettings::GetFloat(bot, "StuckCombatTimeout") * 1000.0f);
    return DungeonClearMath::ShouldBreakStuckCombat(phantom, getMSTime(), timeoutMs,
                                                    stuckCombatSinceMs);
}

bool DungeonClearRegroupCombatTrigger::IsActive()
{
    if (!bot || bot->isDead() || !bot->IsInCombat())
        return false;

    // Feature toggle.
    if (!DcSettings::GetBool(bot, "CombatRegroup"))
        return false;

    // The elected leader tank, non-null only while its clear is active and
    // unpaused. This both gates the feature on an active run and is the bot we
    // regroup on. The leader itself never regroups on anyone — it drives.
    Player* tank = AI_VALUE(Player*, DcKey::PartyTank);
    if (!tank || tank == bot || tank->GetMapId() != bot->GetMapId())
        return false;

    // Hand all advanced-pull camp positioning to the camp/assist actions: while
    // the party is held PASSIVE at a camp (Forming/Advancing/Returning) it must
    // not chase the moving tank or it would break the pull. Once released (Engage,
    // passive == false) or in any non-camp fight, regroup runs — including the
    // healer pre-position case the camp-fight assist (which only engages the pack)
    // misses.
    Position camp;
    bool passive = false;
    if (DcLeaderSignal::GetLeaderCampHold(bot, camp, passive) && passive)
        return false;

    // Gather the game-state reads the pure contribution kernel needs. The verdict
    // (§1) is role-aware: a DPS "can contribute" while its stock LOS-filtered
    // attacker list is non-empty; a healer with a hurt heal target is owned by
    // HealReposition (rel 41), so this only handles its pre-position case.
    DcRegroupDecision::RegroupInputs in;
    in.isHealer = PlayerbotAI::IsHeal(bot);
    in.isMelee  = botAI->IsMelee(bot);
    in.casting  = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL) != nullptr ||
                  bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr;
    in.ccd      = bot->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_FLEEING |
                                    UNIT_STATE_CONFUSED | UNIT_STATE_ROOT);
    in.hasVisibleAttacker = !botAI->GetAiObjectContext()
                                 ->GetValue<GuidVector>(DcKey::Stock::Attackers)->Get().empty();
    // Only meaningful for healers (the kernel ignores it for DPS); skip the party
    // scan otherwise. Non-empty = someone is below the heal floor -> HealReposition
    // owns the reposition, so regroup must stand down (its ownership invariant).
    in.hasHurtHealTarget = in.isHealer &&
                           !AI_VALUE(ObjectGuid, DcKey::HealTarget).IsEmpty();
    in.tankLos     = bot->IsWithinLOSInMap(tank);
    in.tankDist2d  = bot->GetExactDist2d(tank);
    in.healRange   = botAI->GetRange("heal");
    in.hardTether  = DcSettings::GetFloat(bot, "CombatRegroupDistance");
    in.slack       = DcSettings::GetFloat(bot, "CombatRegroupSlack");

    DcRegroupDecision::RegroupVerdict const verdict =
        DcRegroupDecision::DecideCombatRegroup(in);

    uint32 const now = getMSTime();
    bool const wasLatched = _latched;

    // Emergency path: drifted past the hard outer tether. Fire at once, ignore
    // debounce and cooldown, and latch so the close-in is treated as one intent.
    if (verdict == DcRegroupDecision::RegroupVerdict::HardTether)
    {
        _latched = true;
        _pendingSince = 0;
        if (!wasLatched)
            DC_PULL_TRACE("[DC:{}] regroup: verdict=hardtether why=hard-tether "
                          "({:.1f}yd > tether {:.1f}, healer={})",
                          bot->GetName(), in.tankDist2d, in.hardTether, in.isHealer ? 1 : 0);
        return true;
    }

    // Predicate cleared: the bot can contribute (or must not move). Release the
    // latch and, if we were mid-reconnect, start the cooldown so the rung can't
    // immediately re-arm and stutter the bot.
    if (verdict == DcRegroupDecision::RegroupVerdict::None)
    {
        if (wasLatched)
        {
            uint32 const cdMs =
                uint32(DcSettings::GetFloat(bot, "CombatRegroupCooldown") * 1000.0f);
            _cooldownUntil = now + cdMs;
            if (_cooldownUntil == 0)  // reserve 0 as the "no cooldown" sentinel
                _cooldownUntil = 1;
        }
        _latched = false;
        _pendingSince = 0;
        return false;
    }

    // verdict == Reconnect (non-emergency): the bot can't contribute from here.
    // Keep firing if a move is already underway (latched) — one continuous intent.
    if (_latched)
        return true;

    // Suppressed during the post-reconnect cooldown (only HardTether fires then).
    // Signed wrap-safe compare: still cooling while now has not reached the deadline.
    bool const inCooldown = _cooldownUntil != 0 && int32(_cooldownUntil - now) > 0;
    if (inCooldown)
    {
        _pendingSince = 0;  // restart debounce fresh once the cooldown lapses
        return false;
    }

    // Debounce in: the predicate must hold continuously for DC_REGROUP_DEBOUNCE_MS
    // before the first fire, so a one-tick LOS flicker (the tank stepping past a
    // pillar) never launches anyone.
    if (_pendingSince == 0)
        _pendingSince = now ? now : 1;
    if (getMSTimeDiff(_pendingSince, now) >= DC_REGROUP_DEBOUNCE_MS)
    {
        _latched = true;
        DC_PULL_TRACE("[DC:{}] regroup: verdict=reconnect why={} "
                      "({:.1f}yd, los={}, healRange={:.1f})",
                      bot->GetName(), in.isHealer ? "healer-prepos" : "dps-no-attackers",
                      in.tankDist2d, in.tankLos ? 1 : 0, in.healRange);
        return true;
    }
    return false;
}

bool DungeonClearHealRepositionTrigger::IsActive()
{
    if (!bot || bot->isDead())
        return false;

    // Healer-only. Feature toggle.
    if (!PlayerbotAI::IsHeal(bot))
        return false;
    if (!DcSettings::GetBool(bot, "HealReposition"))
        return false;

    // The elected leader tank, non-null only while its clear is active and
    // unpaused. The leader/tank never repositions to heal — it drives the clear.
    Player* tank = AI_VALUE(Player*, DcKey::PartyTank);
    if (!tank || tank == bot || tank->GetMapId() != bot->GetMapId())
        return false;

    // CC'd: can't move anyway; let combat / the trinket handle it.
    if (bot->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_FLEEING |
                          UNIT_STATE_CONFUSED | UNIT_STATE_ROOT))
        return false;

    // Defer to the advanced-pull camp machinery while the party is held passive
    // (Forming/Advancing/Returning) — the camp/assist actions own positioning
    // there, exactly as the regroup trigger defers.
    Position camp;
    bool passive = false;
    if (DcLeaderSignal::GetLeaderCampHold(bot, camp, passive) && passive)
        return false;

    // The most-hurt member, chosen LOS-blind (the whole point — see
    // DungeonClearHealTargetValue). Stored as a GUID (like the pull target),
    // resolved live here. Nothing below the HP floor => nothing to do.
    ObjectGuid const targetGuid = AI_VALUE(ObjectGuid, DcKey::HealTarget);
    if (targetGuid.IsEmpty())
        return false;
    Unit* target = ObjectAccessor::GetUnit(*bot, targetGuid);
    if (!target || !target->IsAlive())
        return false;

    // Defer to visible heal work: if the stock LOS-filtered `party member to heal`
    // resolves to someone genuinely hurt that the bot can already see, let the
    // stock heal stack do its job this tick rather than running off to reposition.
    // We only own the case where the bot has no useful in-sight heal but an
    // out-of-LOS member is dying.
    Unit* visible = AI_VALUE(Unit*, DcKey::Stock::PartyToHeal);
    if (visible && visible->IsAlive() &&
        visible->GetHealthPct() < sPlayerbotAIConfig.mediumHealth &&
        bot->IsWithinLOSInMap(visible))
        return false;

    // Don't sprint across the map into the unknown — a target this far is a
    // wipe/skip case, not a reposition case.
    float const maxRange = DcSettings::GetFloat(bot, "HealRepositionMaxRange");
    if (bot->GetExactDist2d(target) > maxRange)
        return false;

    // Fire only when the target is actually unhealable from here: out of line of
    // sight, or beyond heal range.
    float const healRange = botAI->GetRange("heal");
    return !bot->IsWithinLOSInMap(target) || bot->GetExactDist2d(target) > healRange;
}

bool DungeonClearHazardVacateTrigger::IsActive()
{
    if (!bot || bot->isDead())
        return false;

    // Cheap map early-out before anything else touches game state.
    if (!DcHazardRegistry::HasEmitters(bot->GetMapId()))
        return false;

    // Feature toggle.
    if (!DcSettings::GetBool(bot, "HazardVacate"))
        return false;

    // Can't move out of the pulse if rooted/stunned/etc — don't claim the tick
    // from something that might (a trinket, stock CC break).
    if (bot->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_FLEEING |
                          UNIT_STATE_CONFUSED | UNIT_STATE_ROOT))
        return false;

    // The signal: the bot is standing inside an unfightable emitter's pulse.
    // No combat gate (the Destroyed Sentinel pulses after the kill, often once
    // combat has dropped) and no role exemption (there is nothing to tank — it is
    // NOT_SELECTABLE). Every bot in the pulse leaves.
    return DcHazard::NearestVacate(bot).ok;
}

bool DungeonClearFilterLootTrigger::IsActive()
{
    if (!bot || bot->isDead() || bot->IsInCombat())
        return false;
    Map* map = bot->GetMap();
    if (!map || !map->IsDungeon())
        return false;
    // Fires on EVERY member of a paused run — the leader AND its followers (the
    // helper resolves the run owner cross-context, so it is true for the whole
    // party while paused). Followers are the point: they never set `enabled` and,
    // while paused, "dungeon clear party tank" goes null so the follow-tank
    // action that runs their inline loot filter stops firing — they revert to
    // the stock loot pipeline and grab below-floor junk, which keeps
    // IsAnyPartyMemberLooting true and stalls the tank. Covers ONLY the paused
    // gap: during an active run the inline filters in advance/follow-tank already
    // run. `dc off` clears `enabled`, handing loot fully back to stock.
    return DcLeaderSignal::IsInPausedDungeonClearRun(bot);
}

bool DungeonClearLootRollPendingTrigger::IsActive()
{
    if (!DcSettings::GetBool(bot, "BetterLootRolling"))
        return false;

    // Self-bot: the vote is the human's to cast (BetterLootRollAction casts
    // none), so an open window must not keep the trigger hot.
    if (botAI->IsRealPlayer())
        return false;

    Group* group = bot->GetGroup();
    if (!group)
        return false;

    for (Roll* roll : group->GetRolls())
    {
        auto voteItr = roll->playerVote.find(bot->GetGUID());
        if (voteItr == roll->playerVote.end() || voteItr->second != NOT_EMITED_YET)
            continue;

        // Mirror the action's only no-vote path: it skips a roll whose item
        // template doesn't resolve, so such a roll must not fire the trigger
        // every tick forever.
        if (sObjectMgr->GetItemTemplate(roll->itemid))
            return true;
    }

    return false;
}
