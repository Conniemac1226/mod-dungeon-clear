/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ScriptedPullRegistry.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "Player.h"
#include "Position.h"
#include "Unit.h"

#include "Ai/Dungeon/DungeonClear/DcPullContext.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Playerbots.h"

namespace
{
    // --- Magisters' Terrace (585) — Selin Fireheart's guard packs -------------
    //
    // Room geometry, all live-measured (acore_world.creature, tools/probe_navmesh.py,
    // and the in-game coordinates the design was authored from):
    //
    //   * Selin (24723) at (242.1, 0.3, 1.8). CanAIAttack is X > 216, and his aggro
    //     radius against a level-70 party is ~21yd.
    //   * The room proper opens at X=219; in front of it is a doorway slot at
    //     X 215-218 roughly Y -6..+6 (the Assembly Chamber Door, GO 188065, hangs
    //     at X=215.1), and in front of THAT an empty staging chamber spanning
    //     X 197-213. Everything at |Y| > ~6 with X < 219 is wall.
    //   * Back from the staging chamber a long hall runs down to X~135, where it
    //     opens into the Sunblade antechamber (the last trash before the room:
    //     four elites at X 129-135 / Y -14..-23 and four more at X 119-128 /
    //     Y +15..+21, all of them dead by the time any stage arms).
    //   * EAST pack (the -Y side; `creature_formations` leader 96831): six Wretched
    //     at X 222-230, Y -16..-23, around the fel crystal at (225.97, -20.08).
    //   * WEST pack (the +Y side; leader 96828): six Wretched at X 222-230,
    //     Y +17..+24, around the crystal at (226.31, 20.22).
    //   * CENTRE PAIR (leader 96825): a Skulker at (231.3, 2.8) and a Bruiser at
    //     (232.1, -2.0), ~10yd from Selin. Deliberately NOT a stage — they cannot be
    //     peeled off him, so they belong to the boss pull.
    //
    // Why these coordinates and not others:
    //
    //   CAMP (170.46, 0.57) sits mid-corridor, user-measured, 45.5yd short of
    //   Selin's CanAIAttack plane and 42yd from either stand spot. The number that
    //   makes it work is 55yd — the distance to the nearest guard spawn of either
    //   pack. The pack's two Wretched Husks cast 44503 Fireball / 44504 Frostbolt,
    //   both 40yd, and their smart_scripts rows carry castFlags 64
    //   (SMARTCAST_COMBAT_MOVE), which makes the core switch a mob's combat movement
    //   OFF the moment its target is in range AND in line of sight. So a camp inside
    //   40yd of the room is one a Husk answers by planting in the doorway and
    //   shooting, which is what held the fight open across the doorway at the
    //   original 20yd camp. Past 40 it has no such option and has to run the
    //   corridor, which is what a drag-back is for.
    //
    //   45.5yd under the boss plane is the second number: a Selin tagged by accident
    //   and dragged toward here crosses X=216 long before he arrives, stops being
    //   able to attack anything, and resets. The worst mistake available in this
    //   room is free.
    //
    //   Nearest spawn of any kind is 11.5yd (Blood Knight 96775 of the X 179-182
    //   pack), so the camp is clear floor — checked against `creature`, not by eye.
    //
    //   The camp's own predecessor (209.58, 18.97, -2.05) survives as the ARM
    //   ANCHOR, which is the job it was always doing as well as being the camp: the
    //   tank crosses a 25yd radius of it entering the staging chamber, which is
    //   after the last Sunblade pack (X <= 182.3, 27yd+ out) and before the route
    //   would carry it into the doorway.
    //
    //   The TWO STAND SPOTS are a mirrored pair either side of the doorway's centre
    //   line — same X, same Z, Y negated — and BOTH are outside the room. Each one
    //   shoots a diagonal through the doorway at the pack on the FAR side, and is
    //   walled off from the pack on its own side. That is the whole trick, and it
    //   is why neither pull ever has to set foot past X=216:
    //
    //   EAST STAND (212.22, +7.42) -> the -Y pack. The line to it crosses X=216 at
    //   Y ~ 0, dead centre of the doorway, at 25-27yd: far outside the pack's ~19yd
    //   aggro, hence the ranged tag. The same spot's line to the +Y pack crosses
    //   X=216 at Y ~ +11, which is wall, so the pack 16yd away on its own side
    //   cannot see it.
    //
    //   WEST STAND (212.24, -7.41) -> the +Y pack, the exact mirror. Nearest member
    //   27.8yd (inside a 30yd shield, which is why the tag has to be aimed at the
    //   NEAREST member — see NearestPackMember); Selin 30.8yd; the centre pair
    //   20.2yd but on a line that leaves the doorway at Y ~ -6.3, i.e. wall. The
    //   -Y pack is only 14.6yd from here, which is fine precisely because the stage
    //   ordering guarantees it is dead before this stage ever arms.
    //
    // The pack cylinders are r=12 around each pack's crystal. That holds all six
    // spawns (they span ~8yd) and excludes every neighbour: the centre pair is
    // 22.9yd from the east centre and 18.5yd from the west, and Selin is 25.5 /
    // 25.8yd out. The entry filter is exact — 24688 Skulker / 24689 Bruiser /
    // 24690 Husk exist nowhere else on map 585 — and it is what keeps the fel
    // crystals (24722, faction 190, sitting at the dead centre of both cylinders)
    // from reading as live pack members and stalling the stage forever.
    //
    // ARM RADIUS 25 from the ARM ANCHOR (209.58, 18.97), never from the camp. The
    // camp is 43yd back from the anchor, and a 25yd radius drawn at the camp instead
    // would cover the X 179-182 Sunblade pack 11.5yd away — the stage would arm while
    // that pack is still the run's live problem, take the pull pipeline off it, and
    // pin the party in the middle of it.
    uint32 constexpr MGT_MAP            = 585;
    uint32 constexpr MGT_SELIN          = 24723;
    uint32 constexpr MGT_WRETCHED_SKULK = 24688;
    uint32 constexpr MGT_WRETCHED_BRUIS = 24689;
    uint32 constexpr MGT_WRETCHED_HUSK  = 24690;

    std::vector<ScriptedPullStage> const& Table()
    {
        static std::vector<ScriptedPullStage> const kStages = []
        {
            std::vector<ScriptedPullStage> t;

            ScriptedPullStage east;
            east.mapId      = MGT_MAP;
            east.bossEntry  = MGT_SELIN;
            east.order      = 0;
            east.name       = "Magisters' Terrace — Selin's east guard pack";
            east.campX      = 170.46f;  east.campY  =   0.57f; east.campZ  = -2.72f;
            east.standX     = 212.22f;  east.standY =  7.42f;  east.standZ = -2.82f;
            east.packX      = 225.97f;  east.packY  = -20.08f; east.packZ  = -2.90f;
            east.packRadius = 12.0f;
            east.packZBand  = 12.0f;
            east.armX       = 209.58f;  east.armY   = 18.97f;  east.armZ   = -2.05f;
            east.armRadius  = 25.0f;
            east.entries    = {MGT_WRETCHED_SKULK, MGT_WRETCHED_BRUIS, MGT_WRETCHED_HUSK};
            t.push_back(east);

            ScriptedPullStage west;
            west.mapId      = MGT_MAP;
            west.bossEntry  = MGT_SELIN;
            west.order      = 1;
            west.name       = "Magisters' Terrace — Selin's west guard pack";
            west.campX      = 170.46f;  west.campY  =   0.57f; west.campZ  = -2.72f;
            west.standX     = 212.24f;  west.standY = -7.41f;  west.standZ = -2.82f;
            west.packX      = 226.31f;  west.packY  = 20.22f;  west.packZ  = -2.90f;
            west.packRadius = 12.0f;
            west.packZBand  = 12.0f;
            west.armX       = 209.58f;  west.armY   = 18.97f;  west.armZ   = -2.05f;
            west.armRadius  = 25.0f;
            west.entries    = {MGT_WRETCHED_SKULK, MGT_WRETCHED_BRUIS, MGT_WRETCHED_HUSK};
            t.push_back(west);

            std::stable_sort(t.begin(), t.end(),
                             [](ScriptedPullStage const& a, ScriptedPullStage const& b)
                             {
                                 return a.mapId != b.mapId ? a.mapId < b.mapId
                                                           : a.order < b.order;
                             });
            return t;
        }();
        return kStages;
    }
}

bool ScriptedPullRegistry::HasRows(uint32 mapId)
{
    for (ScriptedPullStage const& s : Table())
        if (s.mapId == mapId)
            return true;
    return false;
}

std::vector<ScriptedPullStage const*> ScriptedPullRegistry::Rows(uint32 mapId)
{
    std::vector<ScriptedPullStage const*> out;
    for (ScriptedPullStage const& s : Table())
        if (s.mapId == mapId)
            out.push_back(&s);
    return out;  // Table() is kept sorted by (mapId, order), so this is ascending.
}

ScriptedPullStage const* ScriptedPullRegistry::Find(uint32 mapId, int32 order)
{
    if (order < 0)
        return nullptr;
    for (ScriptedPullStage const& s : Table())
        if (s.mapId == mapId && s.order == static_cast<uint32>(order))
            return &s;
    return nullptr;
}

bool ScriptedPullRegistry::InPack(ScriptedPullStage const& s, float x, float y, float z)
{
    if (std::fabs(z - s.packZ) > s.packZBand)
        return false;
    float const dx = x - s.packX;
    float const dy = y - s.packY;
    return (dx * dx + dy * dy) <= (s.packRadius * s.packRadius);
}

bool ScriptedPullRegistry::IsPackEntry(ScriptedPullStage const& s, uint32 entry)
{
    return std::find(s.entries.begin(), s.entries.end(), entry) != s.entries.end();
}

bool ScriptedPullRegistry::InArmRange(ScriptedPullStage const& s, float x, float y, float z)
{
    // The row's own anchor when it names one, else the camp (see ScriptedPullStage
    // ::armX for why a far-back camp must not be its own arm gate).
    float const ax = s.HasArmAnchor() ? s.armX : s.campX;
    float const ay = s.HasArmAnchor() ? s.armY : s.campY;
    float const az = s.HasArmAnchor() ? s.armZ : s.campZ;
    if (std::fabs(z - az) > DC_SCRIPTED_PULL_ARM_ZBAND)
        return false;
    float const dx = x - ax;
    float const dy = y - ay;
    return (dx * dx + dy * dy) <= (s.armRadius * s.armRadius);
}

int32 ScriptedPullRegistry::SelectOrder(std::vector<uint32> const& orders,
                                        std::vector<bool> const& live, int32 pinned)
{
    // A committed stage always wins: mid-drag its own volume reads empty (the pack
    // is being hauled out of it), and re-deriving from `live` there would hand the
    // tank the NEXT pack while it is still running home with this one.
    if (pinned >= 0)
        for (uint32 o : orders)
            if (o == static_cast<uint32>(pinned))
                return pinned;

    size_t const n = std::min(orders.size(), live.size());
    for (size_t i = 0; i < n; ++i)
        if (live[i])
            return static_cast<int32>(orders[i]);
    return -1;
}

Unit* ScriptedPullRegistry::NearestPackMember(Player* bot, AiObjectContext* ctx,
                                              ScriptedPullStage const& s)
{
    if (!bot || !ctx)
        return nullptr;
    // Shares the events framework's volume scan (entry-filtered, z-banded, and
    // reachability-probed), so "is this pack still up" means the same thing here
    // as it does for a ClearRadius step.
    //
    // Ranked from the STAND SPOT, not from the bot. The tag is taken from the
    // stand spot, so the only distance that matters is the one the pull spell has
    // to cover once the tank gets there — and at commit time the bot is still out
    // in the corridor, where "nearest to me" can be a mob on the far side of the
    // pack. Live (tr-20260802-215715-3): the bot committed from 38.9yd out, the
    // scan handed it a Bruiser that was 31.6yd from the stand spot, Avenger's
    // Shield reaches 30, so the tag fell through to the generic close-to-aggro-edge
    // walk-in and carried the tank off the spot and into the room. The nearest
    // member to that same spot is ~25yd — comfortably in range.
    //
    // NEVER the pack member a pull already gave up on. An abort hands ONE mob to the
    // normal walk-in engage (DcPullContext::abortTarget), and every ordinary target
    // path honours that — the corridor scan filters it, the sticky latch releases on
    // it, the pull trigger defers on it. This path did not, and being the path that
    // runs AHEAD of all of them that made the abort unescapable rather than merely
    // ignored: the plan kept handing back the one GUID the trigger was refusing, so
    // the maneuver never ran again and nothing could clear either flag.
    //
    // Live (tr-20260803-144046-2): "target ... is the abort target -> defer to normal
    // engage", 1145 times in four minutes, on the same Bruiser, while the party stood
    // parked at the camp.
    //
    // Excluding it here answers both questions the caller asks at once. As a TARGET
    // the stage falls through to the next member of its own pack and the plan carries
    // on; as the DueStage liveness probe it means a stage whose only survivor is the
    // abort target reads as empty and simply does not arm, which is what lets the run
    // walk in and fight the thing instead of re-arming a plan around it.
    DcPullContext const& pull = ctx->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
    Position const stand(s.standX, s.standY, s.standZ);
    return DcTargeting::NearestHostileNearPoint(bot, ctx, s.packX, s.packY, s.packZ,
                                                s.packRadius, s.packZBand, &s.entries,
                                                &stand, pull.abortTarget);
}

ScriptedPullStage const* ScriptedPullRegistry::DueStage(Player* bot, AiObjectContext* ctx)
{
    if (!bot || !ctx)
        return nullptr;

    uint32 const mapId = bot->GetMapId();
    if (!HasRows(mapId))
        return nullptr;

    std::vector<ScriptedPullStage const*> const rows = Rows(mapId);
    if (rows.empty())
        return nullptr;

    // Boss gate: the plan exists to get the party to ONE boss, so it is dormant
    // unless that boss is the run's current objective. This is also what retires
    // it — once Selin is dead the next objective is a different boss and no stage
    // can arm on the way back past his room.
    std::optional<DungeonBossInfo> const next =
        ctx->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
    if (!next.has_value())
        return nullptr;

    DcPullContext const& pull = ctx->GetValue<DcPullContext&>(DcKey::PullContext)->Get();
    int32 const pinned = pull.scriptedStage;

    // An in-flight stage short-circuits everything below it: the boss/arm gates and
    // the live scan all describe whether a pull may START, and this one already has.
    if (pinned >= 0)
        if (ScriptedPullStage const* held = Find(mapId, pinned))
            return held;

    std::vector<uint32> orders;
    std::vector<bool> live;
    orders.reserve(rows.size());
    live.reserve(rows.size());
    for (ScriptedPullStage const* s : rows)
    {
        if (s->bossEntry != next->entry)
            continue;
        if (!InArmRange(*s, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()))
            continue;
        orders.push_back(s->order);
        live.push_back(NearestPackMember(bot, ctx, *s) != nullptr);
    }

    int32 const pick = SelectOrder(orders, live, /*pinned*/ -1);
    return pick < 0 ? nullptr : Find(mapId, pick);
}

bool ScriptedPullRegistry::IsStageTarget(ScriptedPullStage const& s, Unit const* u)
{
    if (!u)
        return false;
    return IsPackEntry(s, u->GetEntry()) &&
           InPack(s, u->GetPositionX(), u->GetPositionY(), u->GetPositionZ());
}
