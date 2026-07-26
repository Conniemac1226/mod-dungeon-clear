/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcEngageGeometry.h"

#include "DcDoorIndex.h"
#include "DcHazard.h"
#include "DungeonClearUtil.h"   // DcTargeting::GetLiveBoss (until DcTargeting moves)
#include "DungeonClearMath.h"
#include "DungeonClearTuning.h"
#include "Ai/Dungeon/DungeonClear/Data/BossPullbackRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/RoomAggroRegistry.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "AttackersValue.h"
#include "CellImpl.h"
#include "Creature.h"
#include "CreatureGroups.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "ItemTemplate.h"
#include "DBCStores.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "LootMgr.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "InstanceScript.h"
#include "LootObjectStack.h"
#include "Map.h"
#include "ModelIgnoreFlags.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "Chat.h"
#include "ServerFacade.h"
#include "Timer.h"
#include "World.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Util/ChunkedPathfinder.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Ai/Dungeon/DungeonClear/Util/NavmeshSnap.h"
#include "Ai/Dungeon/DungeonClear/Value/DungeonClearLiveBossValue.h"

float DcEngageGeometry::AggroRangeOf(Player* bot, Unit* u, float fallback,
                                    float floorYd, float capYd)
{
    if (!bot || !u)
        return fallback;
    if (!DcSettings::GetBool(bot, "DynamicAggroRange"))
        return fallback;

    Creature* c = u->ToCreature();
    if (!c)
        return fallback;            // players/totems: keep the caller's band

    // Core formula: detection range adjusted by level diff, already clamped
    // 5-45yd. Add the creature's reach so the band measures "how close the
    // route must pass for it to pull", not just its notice distance.
    float range = c->GetAggroRange(bot) + c->GetCombatReach();
    if (range < floorYd)
        range = floorYd;
    if (range > capYd)
        range = capYd;
    return range;
}
float DcEngageGeometry::BossEngageRange(Player* bot, AiObjectContext* ctx,
                                        DungeonBossInfo const& boss, float staticRange)
{
    if (!bot || !ctx)
        return staticRange;
    if (!DcSettings::GetBool(bot, "DynamicAggroRange"))
        return staticRange;

    Creature* live = DcTargeting::GetLiveBoss(bot, ctx, boss.entry);
    if (!live)
        return staticRange;         // not loaded yet — use the static fallback

    float const margin =
        DcSettings::GetFloat(bot, "AggroRangeMargin");
    float const floorYd =
        DcSettings::GetFloat(bot, "BossEngageRangeFloor");
    float const capYd =
        DcSettings::GetFloat(bot, "BossEngageRangeCap");

    // Hand off as the tank enters the boss's real aggro bubble: its notice
    // distance + both reaches + a small margin so the engage trigger fires
    // just before the boss would pull on its own.
    float range = live->GetAggroRange(bot) + live->GetCombatReach()
                + bot->GetCombatReach() + margin;
    if (range < floorYd)
        range = floorYd;
    if (range > capYd)
        range = capYd;

    // Room-wide-aggro boss (RoomAggroRegistry): engaging it force-pulls the WHOLE
    // room, so the tank must clear the room from OUTSIDE the boss's aggro sphere
    // and never cross it during the pre-clear. The capped hand-off above is wrong
    // for a big-aggro boss: for Jammal'an (sphere ~33yd) the cap (30) lands "at
    // boss engage" 3yd INSIDE the skirt sphere and only 2yd outside the boss's
    // real wake distance, so the approach creeps straight through that gap and
    // wakes the boss before the room is clear (the live failure). Widen the
    // hand-off to the skirt-sphere edge plus a small overshoot buffer and DROP the
    // cap, so IsAtBossEngage — which drives the engage HOLD (TryEngageHold), the
    // room-clear activation (IsRoomClearActive), and the no-progress valve — all
    // trip while the tank is still clear of the sphere. The boss PULL stays gated
    // by the at-boss trigger until the room is clear (RoomAggroRegistry + trash
    // remaining), so this changes only WHERE the tank stands to clear, not when it
    // pulls.
    if (RoomAggroRegistry::Find(bot->GetMapId(), boss.entry))
    {
        // The single-source sphere (same value the room-trash exclusion and the
        // skirt avoid-ring use). The standoff sits a fixed buffer OUTSIDE it; the
        // buffer is a positive constant (see the static_assert by its definition),
        // so the ordering invariant "tank hold/clear distance > avoid sphere" holds
        // by construction and cannot drift — the regression that woke Jammal'an.
        float const sphere = RoomAggroSphereRadius(bot, live);
        range = std::max(range, sphere + DC_ROOM_AGGRO_STANDOFF_BUFFER);
    }
    return range;
}

float DcEngageGeometry::RoomAggroSphereRadius(Player* bot, Creature* boss)
{
    if (!bot || !boss)
        return 0.0f;
    float const computed = boss->GetAggroRange(bot) + bot->GetCombatReach()
         + boss->GetCombatReach()
         + DcSettings::GetFloat(bot, "AggroRangeMargin")
         + DcSettings::GetFloat(bot, "RoomAggroPathPadding");

    // A flagged boss may carry a per-boss skirt override (RoomAggroBoss::skirtRadius)
    // that WIDENS this avoid sphere when the raw aggro range under-states how far the
    // fight must stay away — a wide CallForHelp, summoned roamers that must not be
    // fought on top of the boss (Sepethrea). Never SHRINKS below the computed sphere.
    float const skirt = RoomAggroRegistry::SkirtOverride(boss->GetMapId(), boss->GetEntry());
    return skirt > computed ? skirt : computed;
}
float DcEngageGeometry::PullCommitRange(Player* bot, Unit* target, float staticRange)
{
    if (!bot || !target)
        return staticRange;
    if (!DcSettings::GetBool(bot, "DynamicAggroRange"))
        return staticRange;

    Creature* c = target->ToCreature();
    if (!c)
        return staticRange;             // players/totems: keep the fixed fallback

    float const margin = DcSettings::GetFloat(bot, "AggroRangeMargin");
    float const floorYd = DcSettings::GetFloat(bot, "PullCommitRangeFloor");
    float const capYd = DcSettings::GetFloat(bot, "PullCommitRangeCap");

    // Stop just as the tank would enter the pack's real aggro bubble: the core's own
    // notice distance (Creature::GetAggroRange, already level/config-scaled and
    // clamped 5-45yd) + both reaches + a small margin, so the commit fires a hair
    // BEFORE the pack would pull on its own. Identical formula to BossEngageRange.
    float range = c->GetAggroRange(bot) + c->GetCombatReach()
                + bot->GetCombatReach() + margin;
    if (range < floorYd)
        range = floorYd;
    if (range > capYd)
        range = capYd;
    return range;
}

namespace
{
    // True if the navmesh path from the bot to (dx,dy,dz) is a complete walk that
    // stays OUTSIDE the boss's aggro sphere — i.e. a usable skirt leg. A leg whose
    // path can't be completed (not PATHFIND_NORMAL) or that plunges back through
    // the sphere (the only route on that side hugs a wall through the boss's aggro
    // range) is rejected so the caller can try the other way around the ring.
    //
    // It also rejects a leg that walks through a hazard emitter's keep-out
    // cylinder (DcHazardRegistry). Both the short-arc and long-arc candidates go
    // through here, so a single check covers the whole ring solver — and the
    // orbit latch then naturally prefers whichever way around is clean of BOTH
    // the boss sphere and the hazard.
    bool SkirtLegIsClean(Player* bot, float dx, float dy, float dz,
                         float bx, float by, float safeRadius)
    {
        PathGenerator gen(bot);
        gen.CalculatePath(dx, dy, dz, /*forceDest*/ false);
        if (gen.GetPathType() != PATHFIND_NORMAL)
            return false;

        Movement::PointsArray const& path = gen.GetPath();
        // Reject a leg that dips well inside the aggro sphere. Skip the first
        // point (the bot's own position, which may legitimately graze the ring
        // edge) and allow a 2yd tolerance so a clean exterior skirt that only
        // kisses the boundary isn't thrown out.
        float const intrudeR = safeRadius - 2.0f;
        if (intrudeR > 0.0f)
        {
            float const intrudeSq = intrudeR * intrudeR;
            for (size_t i = 1; i < path.size(); ++i)
            {
                float const px = path[i].x - bx;
                float const py = path[i].y - by;
                if (px * px + py * py < intrudeSq)
                    return false;
            }
        }

        // Hazard screen: a skirt that swings wide of the boss straight into a
        // damage aura trades one problem for a worse one.
        //
        // Tested as SEGMENTS, not vertices. PathGenerator hands back string-pulled
        // CORNER points, not a densified line — an open-room skirt leg is often
        // just {start, end}, so a per-vertex scan would only ever check the
        // destination and wave through a leg that walks clean across an emitter.
        for (size_t i = 1; i < path.size(); ++i)
            if (DcHazard::SegmentIsHot(bot,
                                       path[i - 1].x, path[i - 1].y, path[i - 1].z,
                                       path[i].x, path[i].y, path[i].z))
                return false;

        return true;
    }
}

std::optional<Position> DcEngageGeometry::AggroSafeApproachPoint(
    Player* bot, float bx, float by, float bz, float safeRadius, Unit* target,
    int8* orbitDir)
{
    if (!bot || !target || safeRadius <= 0.0f)
        return std::nullopt;

    float const tx = bot->GetPositionX();
    float const ty = bot->GetPositionY();
    float const gx = target->GetPositionX();
    float const gy = target->GetPositionY();

    // Distance from the boss centre to the target. USUALLY > safeRadius (kept
    // room-trash sits outside the aggro sphere), but a forced-advanced pull
    // (RoomAggroBoss::pullOutRadius) deliberately KEEPS a pack as room-trash while
    // it stands INSIDE the nominal sphere — the Mechanar dead band, where Pack B
    // sits ~17yd from Nethermancer Sepethrea, inside her ~36yd avoid sphere.
    float const targetDist = std::sqrt((gx - bx) * (gx - bx) + (gy - by) * (gy - by));

    // Skirting a target that sits INSIDE the avoid sphere is the same incoherent
    // infinite-orbit as skirting the boss itself: the straight line bot->target
    // always ends inside the sphere, so the early-out below can never fire and the
    // tank orbits the ring forever (the live Sepethrea room-clear deadlock —
    // "skirting ... before approaching Bloodwarder Centurion" logged every tick,
    // never closing the last yard to pull-commit range). Cap the effective avoid
    // radius just under the target so a dead-band pack resolves to a radial walk-in
    // that stops at the pack. No-op for every ordinary room-aggro boss: their kept
    // trash is always outside safeRadius, so the min() never binds.
    safeRadius = std::min(safeRadius, targetDist - 1.0f);
    if (safeRadius <= 0.0f)
        return std::nullopt;

    // Stand WELL back from the aggro sphere, not hard against it. The party
    // follows the tank imperfectly and cuts the corner, so a tight skirt right on
    // the aggro edge pulls the boss even when the TANK stays clear. Orbit at the
    // sphere + a party buffer: the tank "runs back at an angle" out of aggro, and
    // from that wider stand-off a straight shot at far packs opens up while the
    // trailing party still has margin. Tunable (RoomAggroPartyMargin).
    float const partyMargin = DcSettings::GetFloat(bot, "RoomAggroPartyMargin");

    // Clearance the STRAIGHT approach must keep from the aggro sphere before we
    // stop skirting and walk straight in. Full party margin when the target is far
    // enough to allow it; but a pack sitting only a few yards outside the sphere
    // can't be reached with the full buffer, so cap the test radius just under the
    // target's own distance (else the orbit could never resolve to a straight shot
    // — the target would sit inside the test ring forever, the boss-centre
    // infinite-orbit trap). Never below raw aggro: we always clear the sphere.
    float const earlyOutR = std::max(safeRadius,
        std::min(safeRadius + partyMargin, targetDist - 1.0f));

    // Does the straight 2D approach bot->target already keep that clearance from
    // the boss's aggro sphere? Then no detour — engage directly. Release the orbit
    // latch: the orbit is over (we rounded to the open side / the target moved into
    // the clear), so the next skirt for any target starts unbiased.
    if (!NeedsRoomAggroSkirt(tx, ty, gx, gy, bx, by, earlyOutR))
    {
        if (orbitDir)
            *orbitDir = 0;
        return std::nullopt;
    }

    // Orbit the sphere: step the bot's current bearing (measured from the boss)
    // toward the target's bearing by a capped angular increment and place the
    // waypoint on the safe ring there. Called each tick this walks the tank around
    // the ring; the early-out above ends the orbit the moment a straight shot at
    // the target clears the sphere.
    float const phi = std::atan2(ty - by, tx - bx);   // bot bearing from boss
    float const angG = std::atan2(gy - by, gx - bx);  // target bearing from boss
    // Shortest signed turn phi->angG, in (-pi, pi].
    float const delta = std::atan2(std::sin(angG - phi), std::cos(angG - phi));
    // ~34 degrees/tick keeps each leg short enough that it hugs the ring exterior.
    constexpr float kOrbitStep = 0.6f;
    // Orbit ring: the aggro sphere PLUS the party buffer, so the tank arcs wide
    // around the OUTSIDE with the followers clear of aggro. For a pack that sits
    // closer in than this ring (the edge packs at the room's inner wall), the tank
    // backs out past it to the ring, lines up the bearing, then steps straight IN —
    // a radial leg that clears the sphere — instead of hugging the aggro edge the
    // whole way around. Capped to the room via the navmesh snap below.
    float const wpR = safeRadius + partyMargin;

    // Snap a ring waypoint at `bearing` to the navmesh.
    auto ringPoint = [&](float bearing) -> std::optional<Position>
    {
        float const wx = bx + std::cos(bearing) * wpR;
        float const wy = by + std::sin(bearing) * wpR;
        NavmeshSnap::Result const snap = NavmeshSnap::Snap(bot, wx, wy, bz, 25.0f);
        if (!snap.ok)
            return std::nullopt;
        return Position(snap.x, snap.y, snap.z, 0.0f);
    };

    // Already committed to a rotation direction this orbit? KEEP rounding that
    // way. Re-deriving the short/long choice every tick is what livelocked the
    // tank: it would step the long way once (away from the target, "backward"
    // past the boss), then next tick read the short arc as clean again from the
    // new spot and step right back — bouncing between two ring points forever,
    // unable to commit to going all the way around. With the latch the tank
    // rounds the whole long arc to the open side, where the early-out above fires
    // and releases the latch. The committed leg is returned even when its own
    // probe reads blocked (tolerant): one more step that way is how it escapes a
    // pocket, and the early-out is the only thing that ends the orbit.
    if (orbitDir && *orbitDir != 0)
    {
        if (std::optional<Position> wp = ringPoint(phi + float(*orbitDir) * kOrbitStep))
            return wp;
        // Ring snap failed outright — fall through and re-pick a direction.
    }

    // Two candidate directions around the ring:
    //  - SHORT arc: turn the bot's bearing toward the target (the natural,
    //    shortest detour — correct when that side is open).
    //  - LONG arc: the opposite way around. Needed when the short side is
    //    wall-blocked: the live Jammal'an case orbited the short arc straight
    //    into a wall behind the boss and stalled, hugging the wall INTO his aggro
    //    range. Rounding the open front instead reaches the far packs cleanly.
    float const shortBearing = phi + std::clamp(delta, -kOrbitStep, kOrbitStep);
    float const longBearing = phi - std::copysign(kOrbitStep, delta);
    // Rotation sign (bearing += dir * step) for each arc. Short turns toward the
    // target's bearing (sign of delta); long is the opposite way around.
    int8 const shortDir = (delta >= 0.0f) ? int8(1) : int8(-1);

    std::optional<Position> shortWp = ringPoint(shortBearing);
    if (shortWp && SkirtLegIsClean(bot, shortWp->GetPositionX(),
                                   shortWp->GetPositionY(), shortWp->GetPositionZ(),
                                   bx, by, safeRadius))
    {
        // Short arc converges monotonically (delta shrinks each step) and can't
        // oscillate on its own, so it needs no latch — leaving it unlatched lets
        // the choice re-evaluate to LONG the moment the arc runs into a wall.
        if (orbitDir)
            *orbitDir = 0;
        return shortWp;
    }

    std::optional<Position> longWp = ringPoint(longBearing);
    if (longWp && SkirtLegIsClean(bot, longWp->GetPositionX(),
                                  longWp->GetPositionY(), longWp->GetPositionZ(),
                                  bx, by, safeRadius))
    {
        // Commit: latch the long rotation direction so we round the whole way.
        if (orbitDir)
            *orbitDir = int8(-shortDir);
        LOG_DEBUG("playerbots.dungeonclear",
                  "[DC:{}] room-clear skirt: short arc blocked, rounding the long "
                  "way -> detour ({:.1f}, {:.1f})",
                  bot->GetName(), longWp->GetPositionX(), longWp->GetPositionY());
        return longWp;
    }

    // Neither side validated a clean leg (tight geometry, or the probe couldn't
    // build a full path) — keep the legacy short-arc snap rather than abandoning
    // the skirt and bee-lining through the sphere. May be nullopt, in which case
    // the caller falls through to the direct approach exactly as before.
    return shortWp;
}

bool DcEngageGeometry::NeedsRoomAggroSkirt(float botX, float botY,
                                          float targetX, float targetY,
                                          float bossX, float bossY,
                                          float avoidRadius)
{
    if (avoidRadius <= 0.0f)
        return false;

    // Closest approach of the bot->target chord to the boss centre, squared
    // (no sqrt). Inside the padded radius -> the line clips the sphere -> skirt.
    float const clipSq = DungeonClearMath::DistSqToSegment2D(
        bossX, bossY, botX, botY, targetX, targetY);
    return clipSq < avoidRadius * avoidRadius;
}
namespace
{
    // Shared body for the boss-proximity predicates (IsAtBossEngage at the engage
    // standoff; WithinRoomClearWindow at the wider room envelope): within `range`
    // of the boss AND on a navigable SAME level — not a floor up/down. `live` may
    // be null (boss not loaded), in which case bx/by/bz are the static spawn
    // coords (necessarily far and only reached on the tank's own floor).
    bool WithinBossRangeOnFloor(Player* bot, Creature* live,
                                float bx, float by, float bz, float range)
    {
        if (bot->GetDistance(bx, by, bz) >= range)
            return false;

        // Same-floor fast path: within tolerance the straight-line distance is the
        // real approach distance (slopes/ramps stay under it). Skips the path probe
        // in the overwhelmingly common case.
        float const dz = std::fabs(bz - bot->GetPositionZ());
        if (dz <= DC_Z_LEVEL_TOLERANCE)
            return true;
        if (!live)
            return true;

        // Vertically separated: a 3D-close boss may be a whole floor below/above
        // (the tank on a walkway over Rethilgore, or parked under Fenrus). Straight-
        // line proximity is then a lie — require the NAVIGATIONAL distance to be
        // within range AND to end on the boss's level, so a balcony/overhead boss
        // defers the handoff and Advance keeps walking the route to the right floor.
        PathGenerator gen(bot);
        gen.CalculatePath(bx, by, bz, /*forceDest*/ false);
        if (gen.GetPathType() != PATHFIND_NORMAL)
            return false;
        Movement::PointsArray const& path = gen.GetPath();
        if (path.empty())
            return false;
        if (std::fabs(path.back().z - bz) > DC_Z_LEVEL_TOLERANCE)
            return false;
        return gen.getPathLength() <= range;
    }
}

bool DcEngageGeometry::IsAtBossEngage(Player* bot, AiObjectContext* ctx,
                                      DungeonBossInfo const& boss, float staticRange)
{
    if (!bot || !ctx)
        return false;

    // PULL-BACK boss (BossPullbackRegistry): "at the boss" means AT THE ANCHOR,
    // never at the boss. Ghaz'an lives in open water ~150yd of path away from his
    // anchor, so measuring against his LIVE position would keep this false while
    // the tank stands on the anchor — and Advance, which gates on the same
    // predicate, would keep walking the party toward him until they were all
    // swimming. Measure the anchor instead: arriving there IS the handoff, and the
    // engage action then runs the tag-and-drag that brings the boss to the party.
    // Deliberately skips the live-boss floor probe too — the boss is on the water
    // sheet, a different level by construction.
    if (BossPullbackRegistry::Find(bot->GetMapId(), boss.entry))
    {
        float const engageRange = BossEngageRange(bot, ctx, boss, staticRange);
        return bot->GetDistance(boss.x, boss.y, boss.z) < engageRange;
    }

    Creature* live = DcTargeting::GetLiveBoss(bot, ctx, boss.entry);
    float const bx = live ? live->GetPositionX() : boss.x;
    float const by = live ? live->GetPositionY() : boss.y;
    float const bz = live ? live->GetPositionZ() : boss.z;

    float const engageRange = BossEngageRange(bot, ctx, boss, staticRange);
    return WithinBossRangeOnFloor(bot, live, bx, by, bz, engageRange);
}

bool DcEngageGeometry::WithinRoomClearWindow(Player* bot, AiObjectContext* ctx,
                                             DungeonBossInfo const& boss)
{
    if (!bot || !ctx)
        return false;

    RoomAggroBoss const* room = RoomAggroRegistry::Find(bot->GetMapId(), boss.entry);
    if (!room)
        return false;   // not a room-aggro boss — no room-clear window

    Creature* live = DcTargeting::GetLiveBoss(bot, ctx, boss.entry);
    float const bx = live ? live->GetPositionX() : boss.x;
    float const by = live ? live->GetPositionY() : boss.y;
    float const bz = live ? live->GetPositionZ() : boss.z;

    // The room-clear DRIVER roams the whole room: it orbits the avoid sphere on a
    // ring at safeRadius + RoomAggroPartyMargin (navmesh-snapped, so a few yards
    // wider still), and walks out to packs anywhere inside the room radius. Gate
    // the window on whichever is larger — the room, or the orbit ring — plus a
    // buffer, so the bot NEVER falls out of the room-clear window while legitimately
    // rounding the sphere or reaching a far pack. The tight IsAtBossEngage standoff
    // was inside the orbit ring (37 < 43 for Jammal'an), so the bot running back
    // onto the ring exited the window and was handed to the boss-bound Advance —
    // the live failure. NB this is the DRIVER window; the governor keeps its own
    // narrow keep-out ring so it only HOLDS near the boss, never freezing the
    // approach or a far-pack excursion.
    float const sphere = RoomAggroSphereRadius(bot, live);
    float const orbit = sphere + DcSettings::GetFloat(bot, "RoomAggroPartyMargin");
    float const window = std::max(room->radius, orbit) + DC_ROOM_AGGRO_STANDOFF_BUFFER;
    return WithinBossRangeOnFloor(bot, live, bx, by, bz, window);
}

bool DcEngageGeometry::TankReachedRoomByPath(Player* bot, AiObjectContext* ctx,
                                             DungeonBossInfo const& boss)
{
    if (!bot || !ctx)
        return false;

    RoomAggroBoss const* room = RoomAggroRegistry::Find(bot->GetMapId(), boss.entry);
    if (!room)
        return false;   // not a room-aggro boss — no room-clear envelope

    Creature* live = DcTargeting::GetLiveBoss(bot, ctx, boss.entry);
    float const bx = live ? live->GetPositionX() : boss.x;
    float const by = live ? live->GetPositionY() : boss.y;
    float const bz = live ? live->GetPositionZ() : boss.z;

    // Same envelope as WithinRoomClearWindow so the gate and the driver agree on
    // "in the room".
    float const sphere = RoomAggroSphereRadius(bot, live);
    float const orbit = sphere + DcSettings::GetFloat(bot, "RoomAggroPartyMargin");
    float const window = std::max(room->radius, orbit) + DC_ROOM_AGGRO_STANDOFF_BUFFER;

    // Cheap straight-line reject first: clearly outside the window either way.
    if (bot->GetDistance(bx, by, bz) >= window)
        return false;

    // FORCED navmesh probe (unlike WithinBossRangeOnFloor's same-floor straight-
    // line shortcut): a chamber one wall over is straight-line-close but a long
    // detour by foot. Only a PATHFIND_NORMAL route whose LENGTH is within the
    // window means the tank is genuinely in the room and may begin clearing. A
    // far tank yields an INCOMPLETE route (the boss is past the node budget) or an
    // over-window length — both correctly read as "not in the room yet".
    PathGenerator gen(bot);
    gen.CalculatePath(bx, by, bz, /*forceDest*/ false);
    if (gen.GetPathType() != PATHFIND_NORMAL)
        return false;
    return gen.getPathLength() <= window;
}

bool DcEngageGeometry::IsRangedAttacker(Player* bot, Unit* u)
{
    Creature const* c = u ? u->ToCreature() : nullptr;
    if (!c)
        return false;
    CreatureTemplate const* t = c->GetCreatureTemplate();
    if (!t)
        return false;

    // 1. Caster class. Creatures collapse all casters onto a small set of
    // classes (MAGE in practice); accept the others defensively in case a realm's
    // creature data tags a shadow/elemental caster as PRIEST/SHAMAN/WARLOCK.
    switch (t->unit_class)
    {
        case CLASS_MAGE:
        case CLASS_PRIEST:
        case CLASS_SHAMAN:
        case CLASS_WARLOCK:
            return true;
        default:
            break;
    }

    // 2. Ranged weapon in the virtual ranged slot. Bow / gun / crossbow / wand
    // mean the mob attacks from afar; thrown is excluded (short range — the mob
    // closes to throwing distance, which is effectively melee for our purposes).
    uint32 const rangedItemId =
        c->GetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID + uint32(RANGED_ATTACK));
    if (ItemEntry const* ie = sItemStore.LookupEntry(rangedItemId))
    {
        if (ie->ClassID == ITEM_CLASS_WEAPON &&
            (ie->SubclassID == ITEM_SUBCLASS_WEAPON_BOW ||
             ie->SubclassID == ITEM_SUBCLASS_WEAPON_GUN ||
             ie->SubclassID == ITEM_SUBCLASS_WEAPON_CROSSBOW ||
             ie->SubclassID == ITEM_SUBCLASS_WEAPON_WAND))
            return true;
    }

    // 3. A damaging spell with real reach. Mirrors Creature::reachWithSpellAttack's
    // effect test (school damage / leech / instakill) but without the live mana and
    // distance gating — we want the mob's CAPABILITY, not whether it can cast right
    // now. A max range above the floor means it will plant and cast rather than run
    // in, which is the whole reason to break LOS. Catches a caster the engine tagged
    // with a melee unit_class.
    float const rangeFloor = DcSettings::GetFloat(bot, "PullRangedSpellRangeFloor");
    for (uint32 i = 0; i < MAX_CREATURE_SPELLS; ++i)
    {
        uint32 const spellId = t->spells[i];
        if (!spellId)
            continue;
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            continue;
        bool damaging = false;
        for (uint32 j = 0; j < MAX_SPELL_EFFECTS; ++j)
        {
            uint32 const eff = spellInfo->Effects[j].Effect;
            if (eff == SPELL_EFFECT_SCHOOL_DAMAGE || eff == SPELL_EFFECT_HEALTH_LEECH ||
                eff == SPELL_EFFECT_INSTAKILL || eff == SPELL_EFFECT_ENVIRONMENTAL_DAMAGE)
            {
                damaging = true;
                break;
            }
        }
        if (!damaging)
            continue;
        if (spellInfo->GetMaxRange(false) > rangeFloor)
            return true;
    }

    return false;
}
namespace
{
    // GO entries that are TYPED as GAMEOBJECT_TYPE_DOOR but are encounter
    // force-fields / spell-visual barriers, NOT navigation doors the tank can or
    // should open. They are raised/dropped by the encounter (e.g. Immol'thar's
    // prison dome falls when his five demon crystals are destroyed), the navmesh
    // already routes correctly past/around them, and the bot has no way to "open"
    // one — so left in the door predicates they read as a permanently-shut
    // corridor blocker and the run auto-pauses in front of thin air. Observed in
    // Dire Maul West: the path to Prince Tortheldrin grazes the Magic Vortex
    // doodad (179506) and the run paused "blocked by a door" right after
    // Immol'thar died, though nothing was in the way.
    bool IsNavIgnoredBarrierEntry(std::uint32_t entry)
    {
        switch (entry)
        {
            case 179503:  // Doodad_DireMaulBossForceField01 (Immol'thar prison dome)
            case 179506:  // Doodad_DiremaulMagicVortex01 (Immol'thar magic vortex)
                return true;
            default:
                return false;
        }
    }
}

bool DcEngageGeometry::IsDoorClosed(GameObject const* go)
{
    if (!go || !go->IsInWorld())
        return false;
    GameObjectTemplate const* info = go->GetGOInfo();
    if (!info || info->type != GAMEOBJECT_TYPE_DOOR)
        return false;
    // Authored non-blocking (decorative / always-passable) doors never count.
    if (info->door.ignoredByPathing)
        return false;
    // Encounter force-fields / spell visuals miscatalogued as doors (see above):
    // never a navigation blocker.
    if (IsNavIgnoredBarrierEntry(info->entry))
        return false;
    // Physical truth on this core: a door's collision follows GOState ALONE —
    // GO_STATE_READY has collision ON (blocking), both ACTIVE states have it
    // OFF (passable). The template's startOpen flag plays no runtime role:
    // GameObject::AddToWorld and SetGoState both do
    // EnableCollision(state == GO_STATE_READY). The previous XOR against
    // startOpen misread every startOpen=1 gate spawned ACTIVE (the Stratholme
    // portcullises) as closed, parking the tank in front of an open gate.
    return go->GetGoState() == GO_STATE_READY;
}
bool DcEngageGeometry::IsNavReachable(Player* bot, Position const& p)
{
    if (!bot)
        return false;
    PathGenerator gen(bot);
    gen.CalculatePath(p.GetPositionX(), p.GetPositionY(), p.GetPositionZ());
    return gen.GetPathType() == PATHFIND_NORMAL;
}
bool DcEngageGeometry::ClosedDoorBetween(WorldObject* from, float tx, float ty,
                                         float tz, float /*corridorWidth*/)
{
    if (!from)
        return false;
    Map* map = from->GetMap();
    if (!map)
        return false;

    // GameObject-only line of sight. A door / gate is an M2 (or WMO) GameObject
    // whose collision is inserted into the map's DYNAMIC collision tree ONLY
    // while it is shut — GameObject toggles EnableCollision(state ==
    // GO_STATE_READY), so the instant a door opens its model leaves the tree.
    // Therefore a BLOCKED GameObject-LOS ray means a CLOSED door sits squarely
    // on the line between the two points, judged against the door's real
    // oriented collision mesh.
    //
    // This replaces the old GeoBox-AABB-band test, which was the root of the
    // door whack-a-mole: the AABB can sit several yards off where the navmesh
    // actually threads a doorway (SM Cathedral's Chapel Door) and can't tell a
    // route THROUGH a wide doorway from one running PAST it (SFK's courtyard
    // gate). A real LOS ray has neither failure mode and needs no per-door
    // tuning.
    //
    // VMAP (static walls/floor) is deliberately EXCLUDED: we only want "a closed
    // door is in the way", never "the target is around a static corner" — the
    // navmesh already routes around static geometry, so a static block is never
    // the dungeon-clear blocker, and excluding it stops a target behind a wall
    // beside an OPEN passage from being mistaken for door-blocked.
    //
    // Requires the realm's CheckGameObjectLoS=1 (set here); with it off the call
    // returns clear and the bot falls back to the navmesh's door-blind default.
    // Rays run ~2yd above each endpoint so they cross the door PANEL instead of
    // grazing the floor seam. corridorWidth is kept for ABI but unused — an LOS
    // ray needs no width band.
    return !map->isInLineOfSight(
        from->GetPositionX(), from->GetPositionY(), from->GetPositionZ() + 2.0f,
        tx, ty, tz + 2.0f, from->GetPhaseMask(),
        LINEOFSIGHT_CHECK_GOBJECT_ALL, VMAP::ModelIgnoreFlags::Nothing);
}
bool DcEngageGeometry::ClosedDoorNear(WorldObject* ref, float x, float y, float z,
                                      float radius)
{
    if (!ref)
        return false;
    Map* map = ref->GetMap();
    if (!map)
        return false;

    float const radiusSq = radius * radius;
    float const loZ = z - DC_DOOR_Z_BAND;
    float const hiZ = z + DC_DOOR_Z_BAND;

    // Cached door list; shut-state read fresh per door (see DcDoorIndex).
    for (ObjectGuid const guid : DcDoorIndex::Get(map))
    {
        GameObject* go = map->GetGameObject(guid);
        if (!IsDoorClosed(go))
            continue;

        float const gz = go->GetPositionZ();
        if (gz < loZ || gz > hiZ)
            continue;  // door on another floor — near in plan-view only

        float const dx = go->GetPositionX() - x;
        float const dy = go->GetPositionY() - y;
        if (dx * dx + dy * dy <= radiusSq)
            return true;
    }
    return false;
}
float DcEngageGeometry::DistAlongPathToClosedDoor(
    Player* bot, ChunkedPathfinder::Result const& path,
    float doorX, float doorY, float doorZ, float maxLookAhead)
{
    if (!bot || !path.reachable || path.segments.empty())
        return std::numeric_limits<float>::max();

    // Walk the smoothed polyline accumulating distance FROM THE PATH START, and
    // track two cursors: where the path is closest to the bot (the tank's current
    // progress along it) and where it first enters THIS door's band. Return the
    // forward gap between them — the travel still REMAINING to the doorway.
    //
    // The accumulate-from-the-bot version was wrong: the long-path is anchored
    // where it was built, which falls behind as the tank advances, so walking
    // from the bot counted the already-walked prefix and the returned distance
    // GREW as the tank closed in (it never dropped to the stand-off until the bot
    // physically entered the band, parking it on the door). Subtracting the bot's
    // progress cursor fixes that. Only this one door is tested: scanning every
    // nearby door returned whichever the winding route grazed first (an off-route
    // side door), masking the real blocker.
    float const bandSq = DC_DOOR_BAND * DC_DOOR_BAND;
    float const botX = bot->GetPositionX();
    float const botY = bot->GetPositionY();

    float const zBand = DC_DOOR_Z_BAND;
    float prevX = 0.0f, prevY = 0.0f, prevZ = 0.0f;
    bool havePrev = false;
    float accumulated = 0.0f;     // distance from path start to current point
    float cursorAccum = 0.0f;     // accumulated at the point closest to the bot
    float bestBotDistSq = std::numeric_limits<float>::max();
    float doorAccum = -1.0f;      // accumulated at the band entry

    auto visit = [&](float px, float py, float pz) -> bool
    {
        if (havePrev)
        {
            // Only treat the door as on THIS leg if the leg shares its floor —
            // otherwise the route passing over/under the door (a stacked deck or
            // a ramp) registers a band entry far before the real doorway and
            // parks the tank short.
            bool const onFloor =
                doorZ >= std::min(prevZ, pz) - zBand &&
                doorZ <= std::max(prevZ, pz) + zBand;
            if (onFloor &&
                DungeonClearMath::DistSqToSegment2D(doorX, doorY, prevX, prevY, px, py) <= bandSq)
            {
                doorAccum = accumulated;   // at the START (prev) of the hitting segment
                return true;
            }
            float const dx = px - prevX;
            float const dy = py - prevY;
            accumulated += std::sqrt(dx * dx + dy * dy);
        }
        float const bdx = px - botX;
        float const bdy = py - botY;
        float const bd2 = bdx * bdx + bdy * bdy;
        if (bd2 < bestBotDistSq)
        {
            bestBotDistSq = bd2;
            cursorAccum = accumulated;
        }
        prevX = px;
        prevY = py;
        prevZ = pz;
        havePrev = true;
        return false;
    };

    bool hit = false;
    for (PathSegment const& seg : path.segments)
    {
        if (seg.polyline.empty())
            hit = visit(seg.ex, seg.ey, seg.ez);
        else
            for (G3D::Vector3 const& pt : seg.polyline)
                if ((hit = visit(pt.x, pt.y, pt.z)))
                    break;
        if (hit || accumulated >= maxLookAhead)
            break;
    }

    if (!hit)
        return std::numeric_limits<float>::max();

    // A band entry well BEHIND the bot's progress cursor is a door on the
    // already-walked stretch (opened and passed, or a stale flag) — NOT a
    // blocker ahead. Without this, max(0, behind) clamped it to "at door,
    // 0yd", which instantly parked the run and even fired Use() at a door the
    // bot was nowhere near. The slack covers parking inside the hitting
    // segment itself (cursor legitimately runs a few yards past the entry
    // while standing at the doorway).
    constexpr float BEHIND_SLACK = 15.0f;
    if (doorAccum + BEHIND_SLACK < cursorAccum)
        return std::numeric_limits<float>::max();

    return std::max(0.0f, doorAccum - cursorAccum);
}
bool DcEngageGeometry::IsReachable(Player* bot, float x, float y, float z)
{
    // Delegate to the chunked pathfinder. Strict PATHFIND_NORMAL was too
    // restrictive — most dungeon boss-rooms exceed PathGenerator's ~296yd
    // single-call cap, so the strict check returned false for every
    // medium-distance boss and the tank stalled with "no navigable route"
    // before getting a chance to walk the first chunk.
    //
    // The chunked check accepts a path with any forward progress and lets
    // Advance walk the remaining chunks on subsequent ticks. The position-
    // based stuck detector still catches truly unreachable destinations.
    return ChunkedPathfinder::IsReachable(bot, x, y, z);
}
bool DcEngageGeometry::IsLevelReachable(Player* bot, Unit* u)
{
    if (!bot || !u)
        return false;

    // Within DC_Z_LEVEL_TOLERANCE the candidate is on the bot's own level:
    // slopes, stairs and ramps stay under it within a corridor lookahead, so
    // we trust the caller's 2D corridor/cone/LOS test and skip the probe.
    // Otherwise a genuine other-level mob falls through to the pathfinder
    // check below. (A caller WITHOUT a 2D corridor/cone pre-filter — a bare
    // point-radius seek — must use IsEngageReachable, which always probes; the
    // same-level shortcut here is only safe when the caller already screened 2D.)
    float const dz = std::fabs(u->GetPositionZ() - bot->GetPositionZ());
    if (dz <= DC_Z_LEVEL_TOLERANCE)
        return true;

    return IsEngageReachable(bot, u);
}

bool DcEngageGeometry::IsEngageReachable(Player* bot, Unit* u, bool requireDirect)
{
    if (!bot || !u)
        return false;

    // STRICT reachability for point-radius SEEK selection (event ClearRadius /
    // KillCreature.engage). Unlike IsLevelReachable there is NO same-level fast
    // path: a point-radius scan has no corridor/cone 2D pre-filter to lean on, so
    // a straight-line-near mob in an ADJACENT room or tunnel — same level OR not —
    // would otherwise be trusted and EngageDirected straight into the dividing
    // wall (the Uldaman keeper-hall / Ironaya-seal leak). Always probe the route.

    // Confirm an actual ground path. A single direct probe suffices — seek trash
    // sits inside the event's radius, comfortably under PathGenerator's range cap.
    PathGenerator gen(bot);
    gen.CalculatePath(u->GetPositionX(), u->GetPositionY(), u->GetPositionZ(), /*forceDest*/ false);
    if (gen.GetPathType() != PATHFIND_NORMAL)
        return false;

    // PathGenerator clamps an off-mesh destination to the nearest walkable
    // poly, which on layered geometry can be the bot's *own* floor directly
    // above/below the mob — yielding a bogus NORMAL path that never descends.
    // Require the route to actually end on the candidate's level.
    Movement::PointsArray const& path = gen.GetPath();
    if (path.empty())
        return false;
    G3D::Vector3 const& end = path.back();
    if (std::fabs(end.z - u->GetPositionZ()) > DC_Z_LEVEL_TOLERANCE)
        return false;

    // A complete on-level path already rejects the through-a-wall / wrong-level
    // case. A deliberate seek (requireDirect=false) may legitimately walk a long
    // way to its specific objective creature, so stop here for it.
    if (!requireDirect)
        return true;

    // A path existing is not enough: a mob across a wall (or at the bottom of a
    // ledge) can be 15yd away in a straight line while its real approach is a long
    // ramp/corridor detour — for a room pre-clear that means "wrong region /
    // premature fire". Proactively engaging it from up here sends the tank on that
    // whole detour (or wedges it against the wall when the run-in can't path). Treat
    // such a candidate as NOT reachable for target selection — the route brings
    // the tank to its floor eventually, where the straight distance collapses
    // and it's picked up normally. Mirrors the navigational-distance guard in
    // IsAtBossEngage. The slack term keeps legitimate short around-the-corner
    // detours alive at close range, where the pure ratio is too strict.
    float const straight = bot->GetDistance(u);
    return gen.getPathLength() <=
           std::max(straight * DC_TRASH_DETOUR_RATIO, straight + DC_TRASH_DETOUR_SLACK);
}
bool DcEngageGeometry::ComputeCorridor(Player* bot,
                                       float bx, float by, float bz,
                                       Movement::PointsArray& out)
{
    out.clear();
    if (!bot)
        return false;
    PathGenerator gen(bot);
    gen.CalculatePath(bx, by, bz);
    if (gen.GetPathType() != PATHFIND_NORMAL)
        return false;
    out = gen.GetPath();
    return out.size() >= 2;
}
