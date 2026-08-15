/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcHazardRegistry.h"

#include "Ai/Dungeon/DungeonClear/Util/DungeonClearMath.h"

#include <array>
#include <cmath>

namespace
{
    // ---- the table ------------------------------------------------------
    //
    // The Arcatraz (map 552), entry 20869 "Arcatraz Sentinel". Mechanical elite.
    // SmartAI resets it to REACT_AGGRESSIVE at 40% HP, and it carries three addon
    // auras: 11838 (threat-to-zero, so it does not aggro from threat while idle),
    // 31261 (Permanent Feign Death — the ROOT that pins a DORMANT one in place,
    // REMOVED on aggro so it then chases and melees), and 36716 "Energy Discharge"
    // — SPELL_AURA_PERIODIC_TRIGGER_SPELL, period 1000ms, firing 36717 for 563-937
    // at EffectRadiusIndex 18 = 15.0yd. Nothing removes 36716, so the 15yd pulse
    // runs dormant AND in combat. Heroic (21586) swaps 36716->38828->38829,
    // 938-1562, same 15yd/1s. Five spawns, dormant coords:
    //     (255.498, 158.914, 22.362)   (253.942, 131.881, 22.395)
    //     (264.287, -61.321, 22.453)   (336.514,  27.427, 48.426)
    //     (395.413,  18.195, 48.296)
    // radius 22 = the 15yd pulse plus 7yd of drift margin. Below the 30yd caster
    // range on purpose, so a ranged bot can still hold a line past one.
    //
    // The live Sentinel's <=10% "Explode" (36719 -> 36722, ~5000 in 10yd) is NOT
    // registered as a threat: 36719 also MOD_STUNs the sentinel for its own 6s
    // wind-up, so the party simply bursts the helpless 10%-HP mob down before it
    // detonates. Pulling DPS off it to dodge would keep it alive long enough to
    // actually explode — a self-inflicted wound. So no explode handling here.
    //
    // Entry 21761 "Destroyed Sentinel" — the run-wiper the live one summons on
    // death (event 6 -> spell 37394, which summons the fixed creature 21761 for
    // both normal and heroic). NOT_SELECTABLE (unit_flags 33555200) so the party
    // cannot target/kill it, hostile, and it carries the permanent 36716 -> 36717
    // pulse (15yd, 1s, 563-937). It spawns right where the party just killed the
    // Sentinel and ticks until it despawns, often after combat has ended — so
    // vacateRadius drives an active retreat in BOTH engines. Its `radius` (the
    // camp/route keep-out consumed by PointIsHot) is the RAW 15yd pulse, NOT the
    // padded 22 the live fixture uses: the retreat aims pulse+slack = 19yd, and a
    // padded radius would make PointIsHot reject that point as inside this very
    // emitter's cylinder, so the retreat could never find a clear spot.
    //
    // Entries 21303 "Defender Corpse" / 21304 "Warder Corpse" — proximity bombs.
    // SmartAI event 10 (OOC line of sight, param2 = 8) or event 4 (on aggro) fires
    // actionlist 2130400: cast 36599 "Bloody Explosion" + 36593 "Corpse Burst",
    // then despawn. One-shot, avoidance-only. Tighter 12 = 8yd trigger plus 4yd
    // margin, no route penalty box (12yd is inside ordinary pathing jitter).
    //
    // NOT REGISTERED: the Eredar room's two 45yd auras. Its three spawn points are
    // `creature_multispawn`, each rolling Eredar Soul-Eater (20879, "Entropic
    // Aura" 36784 — 45yd, -25% haste and speed, no damage) or Eredar Deathbringer
    // (20880, "Unholy Aura" 27987/38844 -> 27988/38845 — 45yd, 450 normal / 750
    // heroic SCHOOL_DAMAGE every 2s to every enemy in range), re-rolled on every
    // respawn.
    //
    // The Deathbringer's pulse is genuinely lethal, and a keep-out is still the
    // wrong tool for it: 45yd is wider than the ranges the party works at, so
    // sized honestly it refuses every route through the wing, and sized to obey
    // the threat-1 rule (below caster range) it is a lie — at 30yd you take full
    // damage. There is no standing-off from it and no routing around it. The only
    // answer is to cross and kill, which is a set-piece: ArcatrazEvents.cpp event
    // 2, a ClearRadius over the three spawns filtered to both entries.

    // Two corpse clusters overlap Sentinels — the (264.3,-61.3) Sentinel sits
    // beside a Defender Corpse at (272.1,-59.0), and the (395.4,18.2) Sentinel
    // sits inside the corpse pair (392.1,24.9)/(395.1,27.6) — so routing through
    // either takes the pulse AND trips a bomb. Both are covered by the Sentinel
    // penalty boxes in DcNavPenaltyRegistry.
    //
    // Maraudon (map 349), entry 12222 "Creeping Sludge" — 24 spawns, the single
    // biggest source of wipes in the instance. It carries a PERMANENT
    // creature_template_addon aura, 22638 "Poison Shock": passive, infinite
    // duration, SPELL_AURA_PERIODIC_TRIGGER_SPELL on a 2000ms period firing
    // 22595, which is SPELL_EFFECT_SCHOOL_DAMAGE for 181-221 nature at
    // EffectRadiusIndex 8 = 5.0yd against TARGET_UNIT_DEST_AREA_ENEMY around the
    // sludge. Nothing removes it, so the ~100 dps sphere runs while the sludge is
    // idle as well as in combat — merely walking a follower within 5yd of a
    // sleeping one costs ~200 and pulls it.
    //
    // The sludge is what makes a keep-out the RIGHT tool rather than a lie: at
    // speed_run 0.285714 it moves ~2.0 yd/s, under a third of a player's 7.0, so
    // a bot that stays out of the sphere can never be dragged back into it. That
    // is the opposite of the Eredar Deathbringer note below, where the aura is
    // wider than the party's working ranges and standing off is impossible.
    //
    // radius 8 = the 5yd pulse plus 3yd of drift margin, the same sizing as the
    // Noxious Cloud pool below and for the same reason: wide enough that a camp
    // anchor or a fan-out slot is not planted on the rim, tight enough that 24 of
    // them do not sterilise the corridors the party still has to clear.
    //
    // MELEE NEVER TRADE WITH THIS MOB. That is how the sludge is meant to be
    // fought and it is what the vacate bands encode: melee reach is 3D < 4.75yd
    // against a 5.0yd pulse, so "in melee" and "in the aura" are the same place —
    // there is no stance from which a melee bot can hit it without eating ~100 dps,
    // and against 4-8 of them at once that is 400-800 dps on the melee cluster. The
    // sludge is slow enough that nobody has to accept that trade: the party leaves
    // it standing, the ranged shoot it down, and it never catches anyone.
    //
    // Hence vacateRadius 5 with holdBand 6 (in danger inside 11yd) and
    // retreatSlack 9 (flee to 14yd). The wide hold band is the whole point — it
    // has to outlast the bot's own MoveChase, which will keep pulling it back to
    // 4.75yd. At holdBand 2 the bot would read clear at 7yd, get chased back to
    // 4.75, and oscillate THROUGH the aura taking roughly half the damage for none
    // of the benefit. At 6 the chase never gets it closer than 11 before the
    // retreat pushes it back out to 14, so it orbits the pack and takes nothing.
    //
    // First attempt at this row (S1799) set vacateRadius 0 — avoidance only, on the
    // reasoning that melee must be able to swing. tr-20260815-134844-3 and -5 are
    // what that cost: the tank stood in the sphere for the whole fight and died on
    // the first sludge pull of both runs, ending them at 4m24s and 3m41s.
    //
    // No DcNavPenaltyRegistry counterpart: every Creeping Sludge spawn has
    // MovementType != 0 with wander_distance 1-5, so there is no author-time
    // position to box off. Like the ground pools, the live predicates are the whole
    // route defence here.
    //
    // NOT registered: entry 12221 "Noxious Slime" (27 spawns, same instance).
    // Its creature_template_addon auras column is NULL — it carries no permanent
    // pulse at all, and it runs at normal speed. Its only ground threat is the
    // Noxious Cloud it shares with the sludge, which is a pool row, not a creature
    // row. Giving it a creature keep-out would fence off a mob that is not actually
    // emitting anything.
    constexpr std::array<DcHazardEmitter, 5> kEmitters = {{
        //                    radius  zBand  vacate  hold  slack
        { 552, 20869, /*Arcatraz Sentinel  (fought)      */ 22.0f, 12.0f,  0.0f, 2.0f, 6.0f },
        { 552, 21761, /*Destroyed Sentinel (leave once)  */ 15.0f, 12.0f, 15.0f, 2.0f, 6.0f },
        { 552, 21303, /*Defender Corpse                  */ 12.0f,  8.0f,  0.0f, 2.0f, 6.0f },
        { 552, 21304, /*Warder Corpse                    */ 12.0f,  8.0f,  0.0f, 2.0f, 6.0f },
        { 349, 12222, /*Creeping Sludge    (STAY OUT)    */  8.0f,  6.0f,  5.0f, 6.0f, 9.0f },
    }};

    // ---- the ground-pool table ------------------------------------------
    //
    // Scholomance (map 289), spell 17742 "Cloud of Disease". The Diseased Ghoul
    // (10495, 29 spawns, all on this map) runs SmartAI event 6 (on death) ->
    // action 11 cast 17742 on itself, so the pool lands exactly where the party
    // just killed it. From Spell.dbc: Effect[0] = 27 SPELL_EFFECT_PERSISTENT_AREA_AURA
    // applying aura 3 SPELL_AURA_PERIODIC_DAMAGE, 350 nature damage per 1000ms
    // tick, EffectRadiusIndex 8 = 5.0yd, DurationIndex 18 = 20s. That is up to
    // ~7000 damage to anyone who simply stands where they were fighting, at a
    // level bracket where a party member has a few thousand HP — it is the same
    // shape as the Destroyed Sentinel, one tier smaller.
    //
    // There is no creature to key on: a persistent area aura is a DynamicObject,
    // not a unit, which is why this table exists at all. It is also why there are
    // no DcNavPenaltyRegistry boxes to pair with it — the pool's position is
    // unknown until a ghoul dies, so nothing can be hand-authored at author time
    // and the live predicates carry the whole job.
    //
    // radius 8 = the 5yd pool plus 3yd of drift margin: enough that a camp anchor
    // or standoff is not placed on the rim, small enough that it does not sterilise
    // a Scholomance corridor the party still has to walk. vacateRadius is the RAW
    // 5yd pool, NOT the padded 8 — same rule as the Destroyed Sentinel row above:
    // the retreat aims pulse + VacateRetreatSlack = 11yd, which must fall OUTSIDE
    // this row's own PointIsHot cylinder or the retreat can never find a spot it
    // accepts. The 3yd gap between 8 and 11 is the budget for NavmeshSnap pulling
    // the candidate back toward the pool; do not raise `radius` toward 11 without
    // raising VacateRetreatSlack with it.
    //
    // NOT registered elsewhere despite sharing the spell: 17742 is also cast on
    // death by the Silicate Feeder (15333), and Cloud of Disease exists under two
    // other ids — 29047 (Mummified Headhunter, Zul'Gurub) and 41193 (Mutant War
    // Hound, cast in combat rather than on death). None of those sit on a map
    // dungeon-clear runs today. Adding one is a single row here.
    //
    // Maraudon (map 349), spell 21070 "Noxious Cloud" — the same shape one tier
    // down, and the other half of the slime problem. From Spell.dbc: Effect[0] =
    // 27 SPELL_EFFECT_PERSISTENT_AREA_AURA applying aura 3
    // SPELL_AURA_PERIODIC_DAMAGE, BasePoints 150 + DieSides 1 = 151 nature per
    // 1000ms tick, EffectRadiusIndex 8 = 5.0yd, DurationIndex 18 = 20s — ~3000
    // damage to a bot that just stands where it was fighting, against the ~2-3k HP
    // a party member has in the 45-49 bracket.
    //
    // BOTH slimes drop it, and both drop it twice over:
    //   * Noxious Slime (12221, 27 spawns) — SmartAI event 9 SMART_EVENT_RANGE,
    //     rangeMax 5, repeat 10-15s, and event 6 on death.
    //   * Creeping Sludge (12222, 24 spawns) — the same pair, repeat 22-26s.
    // target_type 1 is SMART_TARGET_SELF, so the pool always lands under the mob,
    // which is to say under whoever is meleeing it — and then again on the corpse,
    // under whoever stops to loot. That on-death copy is the Destroyed Sentinel
    // failure exactly: the party has won the fight, combat has dropped, and nothing
    // else in the AI has any reason to move them off 151/s.
    //
    // One row covers both creatures because the pool is keyed on the spell, not on
    // whatever died to make it.
    //
    // Sizing follows the Cloud of Disease row above unchanged — same 5yd aura, same
    // 8/5 split, same 3yd gap to the 11yd retreat aim point. Do not raise `radius`
    // toward 11 without raising VacateRetreatSlack with it.
    //
    // Unlike Scholomance, the vacate here runs against a pool centred on a LIVE mob
    // the party is still fighting. It settles rather than ping-pongs because the
    // DynamicObject stays at the cast point while the mob keeps chasing: melee
    // clear to 11yd, the slime walks off its own pool after them, and they
    // re-engage on clean ground. The Creeping Sludge's 2.0 yd/s makes that
    // separation slow but certain.
    constexpr std::array<DcGroundHazard, 2> kGroundHazards = {{
        //                   radius  zBand  vacate  hold  slack
        // Cloud of Disease — the pool a dying Diseased Ghoul (10495) leaves.
        { 289, 17742, 8.0f, 6.0f, 5.0f, 2.0f, 6.0f },
        // Noxious Cloud — dropped in combat AND on death by both Maraudon slimes.
        { 349, 21070, 8.0f, 6.0f, 5.0f, 2.0f, 6.0f },
    }};
}

bool DcHazardRegistry::HasEmitters(uint32 mapId)
{
    for (auto const& e : kEmitters)
        if (e.mapId == mapId)
            return true;
    return false;
}

bool DcHazardRegistry::HasGroundHazards(uint32 mapId)
{
    for (auto const& g : kGroundHazards)
        if (g.mapId == mapId)
            return true;
    return false;
}

bool DcHazardRegistry::HasAnyHazard(uint32 mapId)
{
    return HasEmitters(mapId) || HasGroundHazards(mapId);
}

DcHazardEmitter const* DcHazardRegistry::Find(uint32 mapId, uint32 creatureEntry)
{
    for (auto const& e : kEmitters)
        if (e.mapId == mapId && e.creatureEntry == creatureEntry)
            return &e;
    return nullptr;
}

DcGroundHazard const* DcHazardRegistry::FindGround(uint32 mapId, uint32 spellId)
{
    for (auto const& g : kGroundHazards)
        if (g.mapId == mapId && g.spellId == spellId)
            return &g;
    return nullptr;
}

bool DcHazardRegistry::PointInCylinder(float radius, float zBand,
                                       float ex, float ey, float ez,
                                       float px, float py, float pz)
{
    if (radius <= 0.0f)
        return false;
    if (std::fabs(pz - ez) > zBand)
        return false;

    float const dx = px - ex;
    float const dy = py - ey;
    return dx * dx + dy * dy < radius * radius;
}

bool DcHazardRegistry::SegmentClipsCylinder(float radius, float zBand,
                                            float ex, float ey, float ez,
                                            float ax, float ay, float az,
                                            float bx, float by, float bz)
{
    if (radius <= 0.0f)
        return false;

    // Reject only when both endpoints are out of band ON THE SAME SIDE. Two
    // endpoints out of band on OPPOSITE sides means the leg descends straight
    // THROUGH the band — a ramp from the Arcatraz z48 upper tier down toward
    // Zereketh's z-10 chamber passes the emitter's exact z with |dz| large at
    // both ends, and a naive `both out => clean` test would wave it through.
    float const da = az - ez;
    float const db = bz - ez;
    if (std::fabs(da) > zBand && std::fabs(db) > zBand && (da > 0.0f) == (db > 0.0f))
        return false;

    float const clipSq = DungeonClearMath::DistSqToSegment2D(ex, ey, ax, ay, bx, by);
    return clipSq < radius * radius;
}

bool DcHazardRegistry::PointInside(DcHazardEmitter const& e,
                                   float ex, float ey, float ez,
                                   float px, float py, float pz)
{
    return PointInCylinder(e.radius, e.zBand, ex, ey, ez, px, py, pz);
}

bool DcHazardRegistry::SegmentClips(DcHazardEmitter const& e,
                                    float ex, float ey, float ez,
                                    float ax, float ay, float az,
                                    float bx, float by, float bz)
{
    return SegmentClipsCylinder(e.radius, e.zBand, ex, ey, ez, ax, ay, az, bx, by, bz);
}

bool DcHazardRegistry::PointInside(DcGroundHazard const& g,
                                   float ex, float ey, float ez,
                                   float px, float py, float pz)
{
    return PointInCylinder(g.radius, g.zBand, ex, ey, ez, px, py, pz);
}

bool DcHazardRegistry::SegmentClips(DcGroundHazard const& g,
                                    float ex, float ey, float ez,
                                    float ax, float ay, float az,
                                    float bx, float by, float bz)
{
    return SegmentClipsCylinder(g.radius, g.zBand, ex, ey, ez, ax, ay, az, bx, by, bz);
}
