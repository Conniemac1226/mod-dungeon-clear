/*
 * mod-dungeon-clear — DcSettingsRegistry.h
 *
 * The single source of truth for every DungeonClear tunable. Each row pairs a
 * config key with its type, default, clamp range, and whether players may
 * override it from the companion addon. Everything else — the conf default
 * lookup, the validation/clamping of client-supplied overrides, and (if the
 * addon panel is schema-driven) the UI controls — derives from this table.
 *
 * Adding a new option later is a one-line change here plus the matching line in
 * mod_dungeon_clear.conf.dist; read it at the use site via DcSettings::GetT().
 * Never reach past this table to sConfigMgr — a raw GetOption re-logs "Config:
 * Missing property ..." on every call, which floods Server.log from any per-tick
 * call site. tools/check_config_reads.py fails the build if one creeps back in.
 *
 * The `key` is the suffix only — the "DungeonClear." prefix is added by the
 * accessor when it falls back to sConfigMgr.
 */

#ifndef _DUNGEON_CLEAR_DC_SETTINGS_REGISTRY_H
#define _DUNGEON_CLEAR_DC_SETTINGS_REGISTRY_H

#include <cstddef>
#include <string_view>

enum class DcType
{
    Bool,
    UInt,
    Int,
    Float
};

struct DcSettingDef
{
    char const* key;          // config key suffix, e.g. "BossEngageRangeFloor"
    DcType      type;
    double      defVal;       // fallback when the conf line is absent
    double      minVal;       // clamp floor for client-supplied overrides
    double      maxVal;       // clamp ceiling
    bool        playerFacing; // exposed to the addon UI + accepts overrides?
};

// The registry. Player-facing rows are overridable per dungeon run; server-only
// rows live here purely so their default is defined in one place (the accessor
// rejects overrides for them and the addon hides them).
inline constexpr DcSettingDef kDcSettings[] =
{
    { "LootMinQuality",        DcType::UInt,   0,   0,   6,  true  },

    // Better Loot Rolling. Master toggle for a set of fixes to mod-playerbots'
    // automatic group-loot rolling. Server-only: read straight from conf by the
    // playerbots loot-roll action (no per-run override — it governs self-bot
    // rolling everywhere, not just inside a dungeon run). First improvement: a
    // bot in "bot self" mode (master == bot, the human's own character on
    // autopilot) no longer casts an automatic Need/Greed vote, since bot and
    // human share one GUID and the bot's vote pre-empts the player's roll
    // dialog. With this on, only the player rolls. OFF preserves stock rolling.
    { "BetterLootRolling",     DcType::Bool,   0,   0,   1,  false },

    { "IgnoreChests",          DcType::Bool,   1,   0,   1,  true  },

    // Seconds the tank may sit in the Blocked state working one closed door
    // (clicking it / holding beside it) before giving up and auto-pausing the
    // run, exactly as it does for a door it knows it can't open. Covers the
    // doors the template-level entitlement check gets wrong: event gates that
    // wear a plain empty-lock template (SFK's Arugal's Lair), and wide gates
    // whose GO origin sits outside click range of the path-side parking spot.
    // The pause stashes the door, so the run still auto-resumes the moment the
    // door really opens (event completes, or a player opens it).
    { "DoorBlockedTimeout",    DcType::UInt,   5,   3, 120,  true  },
    { "RestHealthPct",         DcType::UInt,   0,   0, 100,  true  },
    { "RestManaPct",           DcType::UInt,   0,   0, 100,  true  },

    // Smart Rest: hysteresis full-rest cycles instead of constant micro-rests.
    // When ON, the party pushes with NO eating/drinking at all until any member
    // drops below its role trigger (SmartRestHealthPct for HP, any role;
    // SmartRestDpsManaPct for DPS mana users; SmartRestTankManaPct for tanks;
    // SmartRestHealerManaPct for healers) — then the whole party stops and
    // rests to full health and each enabled mana role before pushing again.
    // While ON, the legacy RestHealthPct/RestManaPct
    // targets above are ignored everywhere. A trigger of 0 disables that
    // mana dimension for both entry and release. Boss pulls top enabled mana
    // roles off to the release bar; roles configured at 0 remain ignored. OFF =
    // the legacy rest behavior, untouched.
    { "SmartRest",              DcType::Bool,   0,   0,   1,  true  },
    { "SmartRestHealthPct",     DcType::UInt,  50,   0, 100,  true  },
    { "SmartRestDpsManaPct",    DcType::UInt,   0,   0, 100,  true  },
    { "SmartRestTankManaPct",   DcType::UInt,  10,   0, 100,  true  },
    { "SmartRestHealerManaPct", DcType::UInt,  40,   0, 100,  true  },

    // Post-combat party resurrection. With PostCombatRez on, a death no longer
    // ends the run outright: when combat drops with dead member(s) and a living
    // member's class can resurrect (Priest/Paladin/Shaman/Druid), the run HOLDS
    // — an elected bot rezzer walks to the corpse and casts (a human-only
    // rezzer is prompted instead) — and resumes once nobody is dead. The run
    // still disables on a full wipe, when no living member can rez, or when the
    // out-of-combat recovery clock exceeds PostCombatRezTimeoutSecs (combat
    // time never burns the budget). The timeout is generous by design: it must
    // cover the rezzer drinking back the mana to afford the cast. OFF restores
    // the classic disable-on-first-death. See Util/DcRezDecision.h.
    { "PostCombatRez",            DcType::Bool,   1,  0,   1,  true  },
    { "PostCombatRezTimeoutSecs", DcType::UInt,  90, 10, 600,  true  },

    // Stranded-member recovery failsafe. The dominant way a run stalls now is a
    // party member falling THROUGH the world geometry (or wedging where the
    // navmesh can't recover): it drifts out of range, the between-pulls spread
    // gate then holds the tank forever waiting for it to catch up, and the whole
    // run freezes. With StrandedRecovery on, when the run has shown NO progress —
    // no boss/objective completed and the tank not closing on the next anchor —
    // for StrandedRecoveryNoProgressSecs while a BOT member is stuck beyond
    // PartyMaxSpread of the tank, that member is teleported to the tank (bots
    // only; a human is never relocated). The long clock is deliberate: it must
    // never fire during a legitimately slow pull/rest, only a true freeze, and
    // combat re-arms it (a fight is progress) so a long boss fight never trips it.
    // OFF disables the failsafe entirely. See Util/DcStrandedDecision.h +
    // DcStrandedRecovery.
    { "StrandedRecovery",              DcType::Bool,   1,   0,    1,  true  },
    { "StrandedRecoveryNoProgressSecs", DcType::UInt,  60,  60, 3600,  true  },

    // Wait at Boss: auto-pause the run at the moment the tank would commit a
    // boss pull and hold for the human's resume (the addon Pause/Resume button
    // or `dc pause`), so the party can prepare instead of the tank rushing in
    // unannounced. Once per boss per run. See DcWaitAtBossDecision.h.
    { "WaitAtBoss",             DcType::Bool,   0,   0,   1,  true  },

    { "PreventBotRelease",     DcType::Bool,   1,   0,   1,  true  },

    // Diagnostics: record every boss-approach decision (the pure DecideApproach
    // observation + verdict) to a JSONL capture file for the offline replay
    // harness. OFF by default — turn it on only to freeze a live freeze/stutter
    // into a permanent regression fixture (see t/replay_decisions.cpp). The
    // capture path is dungeonclear_decisions.jsonl in the worldserver's working
    // dir, overridable via the DUNGEONCLEAR_DECISIONS_FILE env var.
    { "RecordDecisions",       DcType::Bool,   0,   0,   1,  true  },
    { "PartyMaxSpread",        DcType::Float, 25,  10,  60,  true  },

    // In-combat regroup (contribution-gated, Option B): reconnect a follower to the
    // fight ONLY when it truly can't contribute from where it stands — a DPS with no
    // visible attacker, or a healer parked where it couldn't heal the tank when
    // damage starts — and walk it to a role-correct standoff point with LOS on the
    // fight (never onto the tank). CombatRegroup is the master toggle.
    // CombatRegroupDistance is now a HARD OUTER TETHER: past it a follower reconnects
    // regardless of the contribution test (the drifted-into-nowhere safety net),
    // bypassing debounce/cooldown. CombatRegroupSlack is subtracted from heal range
    // in the healer pre-position test (stand a little inside range). CombatRegroup-
    // Cooldown (seconds) is the re-arm delay after a completed reconnect so the rung
    // can't flap. See DungeonClearRegroupCombat{Trigger,Action} + DcRegroupDecision.
    { "CombatRegroup",         DcType::Bool,   1,   0,   1,  true  },
    { "CombatRegroupDistance", DcType::Float, 40,  15, 100,  true  },
    { "CombatRegroupSlack",    DcType::Float,  8,   0,  20,  true  },
    { "CombatRegroupCooldown", DcType::Float,  5,   0,  30,  true  },

    // Healer LOS reposition. The real fix for a healer that stops healing once
    // the tank is dragged out of line of sight: stock playerbots drops an
    // out-of-LOS member from its heal-target value entirely, so the healer neither
    // heals the tank nor moves to it. With HealReposition on, a healer whose
    // most-hurt heal target (tank-biased) is below HealRepositionHpFloor but
    // unhealable from where it stands (out of LOS or > heal range) walks to a
    // point with line of sight + heal range, after which the stock heal stack
    // re-acquires it. HealRepositionTankBias is the health% the leader tank is
    // favoured by when choosing whom to chase (it is the one being kited).
    // HealRepositionMaxRange caps how far the healer will chase before treating it
    // as a wipe/skip rather than a reposition. See DungeonClearHealReposition{
    // Trigger,Action} and DungeonClearHealTargetValue.
    { "HealReposition",        DcType::Bool,   1,   0,   1,  true  },
    { "HealRepositionHpFloor", DcType::Float, 90,   1, 100,  true  },
    { "HealRepositionTankBias",DcType::Float, 15,   0,  50,  true  },
    { "HealRepositionMaxRange",DcType::Float, 60,  20, 120,  true  },

    // Hazard vacate (DcHazardRegistry, threat 2). Master toggle for actively
    // moving OUT of an unfightable persistent-pulse creature — the Arcatraz
    // "Destroyed Sentinel" (21761), summoned at a Sentinel's corpse, NOT_SELECTABLE,
    // pulsing 15yd/1s until it despawns. With this on, any party bot standing in
    // the pulse walks clear and the run then advances past the corpse. OFF leaves
    // bots standing on the corpse taking the pulse (a likely wipe). See
    // DungeonClearHazardVacate{Trigger,Action}.
    { "HazardVacate",          DcType::Bool,   1,   0,   1,  true  },
    // Room-wide-aggro pre-clear (RoomAggroRegistry). ClearRoomBeforeBoss is the
    // master toggle: for the handful of bosses that force the whole room into
    // combat on engage (SM Cathedral, Shadow Lab, Pandemonius, Dagran, …), clear
    // that room BEFORE pulling the boss instead of eating the pile. The clear
    // honours the chosen pull type. RoomClearTimeout is the no-progress give-up:
    // if the remaining room trash hasn't dropped for this many seconds the tank
    // stops holding and pulls the boss anyway, noting it in chat. The clock only
    // runs WHILE the tank is at the boss and actively clearing (it's re-armed
    // during the walk-in), so this measures a true stall — an unreachable
    // straggler or respawn churn — not the time to clear. It must therefore
    // tolerate a slow pack plus a between-pulls drink/rest, hence the generous
    // default. 0 = never give up. Max 600s. (Old 30s default tripped before the
    // tank even reached the room.)
    { "ClearRoomBeforeBoss",   DcType::Bool,   1,   0,    1,  true  },
    { "RoomClearTimeout",      DcType::UInt, 180,   0,  600,  true  },
    // Extra yards added to a room-aggro boss's avoid-sphere when the tank routes
    // AROUND it to reach a trash pack (DcEngageGeometry::AggroSafeApproachPoint).
    // The room-trash EXCLUSION sphere is sized to the boss's exact aggro range +
    // reaches + AggroRangeMargin; the orbiting APPROACH wants a little more slack
    // on top so the tank skirts comfortably outside aggro instead of grazing it.
    { "RoomAggroPathPadding",  DcType::Float,  3,   0,   30,  true  },
    // How far OUTSIDE a room-aggro boss's aggro sphere the tank arcs when skirting
    // around it to reach room trash (DcEngageGeometry::AggroSafeApproachPoint). The
    // tank itself only needs to clear aggro, but the party follows imperfectly and
    // cuts the corner — a tight skirt on the aggro edge pulls the boss anyway. This
    // buffer makes the tank "run back at an angle" to a wider stand-off so the
    // trailing followers stay clear and a straight shot at far packs opens up. Too
    // large wastes travel / can push the ring into a wall (the navmesh snap pulls
    // it back in); too small risks the party clipping aggro.
    { "RoomAggroPartyMargin",  DcType::Float, 10,   0,   40,  true  },

    // Travel-objective anchors (BossRosterRegistry, e.g. Sunken Temple event
    // waypoints): the default arrival radius at which a non-combat objective is
    // marked done and the clear advances. A roster row may override per-anchor
    // (DungeonBossInfo::arriveRadius); 0 there falls back to this.
    { "ObjectiveArriveRadius", DcType::Float,  8,   3,   40,  true  },

    // Per-step timeout (seconds) for the declarative dungeon-event executor
    // (DungeonEventRegistry / DungeonEventExecutor): a single step that keeps
    // returning Running for this long is treated as failed — a required event
    // then stalls for the human, an optional one is skipped and the clear
    // advances. Used when a step doesn't set its own timeout. Generous so a slow
    // approach + a scripted sequence isn't cut short.
    { "EventStepTimeout",      DcType::UInt,  30,   5,  300,  true  },

    { "BossEngageRangeFloor",  DcType::Float, 12,   5,  40,  true  },
    { "BossEngageRangeCap",    DcType::Float, 30,  10,  60,  true  },
    { "TrashWidthFloor",       DcType::Float,  8,   4,  30,  true  },
    { "TrashWidthCap",         DcType::Float, 30,  10,  60,  true  },
    { "DynamicAggroRange",     DcType::Bool,   1,   0,   1,  true  },
    { "AggroRangeMargin",      DcType::Float,  2,   0,  10,  true  },

    // Advanced pull (LOS pull-to-camp). Setback is how far BACK along the cleared
    // route the camp is placed (and therefore how far the tank drags the pack) —
    // dungeon mobs have no leash, so this is purely "how much room the party
    // gets". SafeRadius is the clearance the camp keeps from any OTHER pack so the
    // fight can't aggro a neighbour; if the setback point isn't clear the placer
    // walks further back (up to MaxDrag) until it is. See ComputeSafeCamp.
    { "PullSetback",           DcType::Float, 25,  10, 100,  true  },
    { "PullCampSafeRadius",    DcType::Float, 25,  12,  60,  true  },
    { "PullMaxDrag",           DcType::Float, 35,  20, 200,  true  },

    // Ranged LOS-break pull. When the pulled pack has a ranged attacker (caster,
    // archer, wand — see DcEngageGeometry::IsRangedAttacker) it would otherwise
    // stand at the room's edge and plink the party across open ground. With
    // PullRangedLosBreak on, ComputeSafeCamp keeps walking the camp BACK along the
    // cleared route until it finds a point with no line of sight to the pack —
    // typically the doorway/corner the tank entered through — so the rangers are
    // forced to close to melee at camp. PullRangedMaxDrag is the (larger) drag cap
    // used only for these pulls, since the corner can sit well beyond the normal
    // PullMaxDrag; if no out-of-sight point is reachable within it the placer falls
    // back to the farthest cleared point (best effort — LOS can't always be broken).
    // PullRangedSpellRangeFloor is the spell max-range above which a damaging
    // creature spell counts as "fights at range" (server-only tuning detail).
    { "PullRangedLosBreak",        DcType::Bool,   1,   0,   1,  true  },
    { "PullRangedMaxDrag",         DcType::Float, 60,  20, 250,  true  },
    { "PullRangedSpellRangeFloor", DcType::Float, 15,   8,  40,  false },

    // Seconds the party stays passive AFTER the leader commits the pull (flips to
    // Engage) before DPS are freed to fight — gives the tank a threat head start.
    // Only the graceful Engage commit is delayed; ending/pausing the run or the
    // camp-safety valve release at once. 0 = release the party immediately.
    { "PullPlayerReleaseDelay", DcType::Float, 1.5,  0,  10,  true  },

    // Threat-lead panic bypass (DungeonClearMath::ShouldReleaseFollower). On the
    // assist path (Leeroy walk-ins / unplanned aggro / general combat), DPS are
    // held for the PullPlayerReleaseDelay lead after the leader enters combat to
    // give the tank a threat head start. If the tank's HP drops below this percent
    // it is LOSING the fight — release the party at once regardless of the lead.
    // 0 disables the bypass (always honour the full lead). Healers always bypass.
    { "PullThreatLeadPanicHp",  DcType::Float, 60,   0, 100,  true  },

    // Camp-safety valve for advanced pull mode (`dc pull`). While a pull is in
    // progress the DPS and healer wait passive at the camp and can't defend
    // themselves if a patrol clips the camp or the pull goes sideways. If a held,
    // passive party member is in combat and drops below this health percent, the
    // pull is aborted and the whole party is released to fight back. 0 disables
    // the valve. See DcFollowerLifecycle::ReapStrandedPassives.
    { "PullSafetyHpPct",        DcType::Float, 50,   0, 100,  true  },

    // Hysteresis (seconds) on the cross-bot "is the party fighting?" gate that
    // drives BOTH the dynamic scout-lag suppression and the fight-assist arm. A
    // bare leader->IsInCombat() read is a point-in-time check, and combat starts
    // OR drops on ticks we do not control (a wandering mob aggros the tank between
    // pulls; a pulled pack leashes out of LOS for a tick; the tank tags-and-
    // repositions) — a TOCTOU race. Without hysteresis a single false reading
    // snaps the whole party from "collapse and help" back to the far scout-lag
    // ring and out of the fight, then back, while the tank fights at low HP. Once
    // any party member is SEEN in combat the engaged verdict is held for this many
    // seconds, so a lone stale/false reading can never drop the party out of help
    // mode. 0 disables the latch (bare instantaneous check — not recommended).
    { "PartyCombatLatch",       DcType::Float, 3,    0,  15,  true  },

    // Phantom-combat escape hatch. A DC member can be left FLAGGED in combat by a mob
    // that spawned across the map / behind a gate (a proximity/gate event spawn) and
    // tagged it: the core CombatManager reference never drops because the holder is
    // UNREACHABLE (no navmesh path to it), DC's own gates that key off "someone is in
    // combat" then spin forever, and a `dc off`/`on` can't clear it (the flag isn't
    // DC's). ONLY when a member is in combat, nothing is meleeing it, it has no victim,
    // AND every unit holding it in combat is unreachable-by-path or evading — for
    // StuckCombatTimeout seconds — does DC force-clear its combat + threat (the effect
    // of a GM `.combatstop`). Keying on REACHABILITY, not distance, is what makes it
    // safe: a fleeing/kiting party's pursuers are always path-reachable, so it never
    // fires there; a bot with combat forced by a script that leaves no unit reference
    // is likewise never touched; and it is disabled outright in RAID zones (where an
    // errant drop could reset a boss). The timeout is LONG by default so an encounter
    // that intentionally holds the party in combat is never mistaken for a stuck flag;
    // 0 disables the recovery. See DungeonClearBreakStuckCombatTrigger +
    // DungeonClearMath::ShouldBreakStuckCombat.
    { "StuckCombatTimeout",     DcType::Float, 15,   0, 120,  true  },

    // Seconds a follower's pet stays passive AFTER its owner is released (on top
    // of PullPlayerReleaseDelay). Releasing pet and owner in lockstep lets the
    // pet charge in and pull aggro off the tank before he's settled, botching the
    // pull; the delay lets the tank establish threat first. 0 = release at once.
    { "PullPetReleaseDelay",   DcType::Float, 2.5,  0,  10,  true  },

    // CC-assist: when the leader tank is CC'd mid-pull while dragging the pack to
    // camp (stunned / feared / confused / rooted, or slowed below PullCcSlowFloor
    // of base run speed), the drag fails — the tank can't retreat and just eats
    // the pack while the party stands passive at camp. PullCcAssist (master
    // toggle) aborts that pull the instant the CC has lasted PullCcAssistGrace
    // seconds, dropping the party out of its passive hold to pile onto the pack and
    // help (via the existing camp-fight assist). The grace ignores a brief micro-CC
    // so a 0.5s stutter-stun doesn't throw an otherwise-fine pull away; sustained
    // CC (the pull IS failing) releases the party. Daze is already immunized for
    // the pull, so a slow detected here is a real debuff (Hamstring, web, frost).
    // See DungeonClearPullManeuverAction + DungeonClearMath::ShouldAbortPullForCc.
    // Turn-and-plant on the drag-back (DungeonClearPullManeuverAction +
    // DungeonClearMath::ShouldPlantEarly). A human tank dragging a pack to camp
    // doesn't sprint the WHOLE leg back-turned — once the pack is glued and chasing
    // it stops a few steps in, turns, and fights wherever it plants. PullPlantEnable
    // is the master toggle. PullPlantGlueRadius is the all-attackers-within radius
    // that arms the plant: the pack is gathered and will close wherever the tank
    // stops. Suppressed for LOS-break pulls (those must reach the corner) and gated
    // on at least half the return leg covered. The plant point becomes the new camp.
    { "PullPlantEnable",       DcType::Bool,   1,   0,    1,  true  },
    { "PullPlantGlueRadius",   DcType::Float, 6.0,  2,   20,  true  },

    { "PullCcAssist",          DcType::Bool,   1,   0,    1,  true  },
    { "PullCcAssistGrace",     DcType::Float, 1.0,  0,   10,  true  },
    { "PullCcSlowFloor",       DcType::Float, 0.6,  0.1,  1,  true  },

    // PullCommitRange{Floor,Cap}: how close the pack must be before the tank stops,
    // holds, and waits for the party at camp BEFORE stepping in to tag. Sized to the
    // pack's REAL aggro radius (Creature::GetAggroRange + reaches + AggroRangeMargin
    // — the same exact core value the boss handoff uses) so the tank Forms just
    // OUTSIDE aggro instead of face-pulling mid-glide. Clamped to [floor,cap]; the
    // cap stays inside the ~35yd pull-detection band. Honoured only while
    // DynamicAggroRange = 1; otherwise the fixed fallback applies.
    { "PullCommitRangeFloor",  DcType::Float, 16,   5,  40,  true  },
    { "PullCommitRangeCap",    DcType::Float, 34,  10,  60,  true  },

    // Dynamic pull (setting 2): the tank auto-picks Leeroy vs Advanced per pack by
    // ESTIMATING how many mobs aggro if it Leeroys on top of the target — proximity
    // aggro from each mob's own level-scaled aggro radius plus one CallForHelp
    // assist hop (see DcPullPlanner::ClassifyPullAdvanced and DungeonClearMath::
    // EstimateAggroCount). MaxLeeroyMobs is the party's comfortable simultaneous-
    // mob ceiling: an estimate ABOVE it => Advanced (peel one cluster at a time),
    // at/below => Leeroy. This single count is the whole verdict and self-tunes per
    // zone/level because the reach comes from the real creature aggro radius, not a
    // hand-set chain distance. (Replaces PullDynamicChainRadius +
    // PullDynamicLargePackThreshold, both removed.)
    { "PullDynamicMaxLeeroyMobs",   DcType::UInt,   5,  1,  20,  true  },
    // CombatSpread pads every proximity reach to model the party drifting to
    // flank/kite during the fight (the camp is a disc, not a point). This is a
    // zone-independent fudge for player movement, NOT a per-zone distance, so one
    // default holds everywhere; higher = counts mobs slightly farther out = more
    // cautious. (The assist-hop reach is NOT a setting — it reads the engine's own
    // CreatureFamilyAssistanceRadius directly, see ClassifyPullAdvanced.)
    { "PullCombatSpread",           DcType::Float,  6,  0,  20,  true  },

    // Dynamic pull only: how far BACK the party trails the tank while it scouts
    // toward the next pack and sizes up the Leeroy/Advanced verdict (leader out of
    // combat, pull phase Idle). The normal ~6yd follow bubble would trail the party
    // right onto the tank's heels and into the pack's aggro arc before the tank had
    // committed, accidentally triggering the pull. This wider lag keeps the party a
    // safe distance back so the tank reaches aggro range alone, decides, and only
    // then does the party arrive (it holds at camp for Advanced, or catches up to
    // charge once the tank commits the Leeroy). See DungeonClearFollowTankAction.
    { "PullDynamicPartyLag",   DcType::Float, 15,   6,  40,  true  },
    // Dynamic pull only: Leeroy roll-in. How far OUTSIDE the tank's commit range
    // (yd) the scout lag above releases when the standing verdict is Leeroy — the
    // tank is committing to the charge, so the party closes the gap DURING its
    // final approach and arrives roughly with first contact, instead of standing
    // flat-footed at the lag ring until combat registers and only then starting a
    // 15-20yd run (the 2-3s "bots watching their tank fight" beat on every Leeroy).
    // 0 = release only once the tank reaches commit range; larger = the party
    // rolls earlier alongside the tank. See DcLeaderSignal::IsLeaderDynamicScouting.
    { "PullDynamicRollInLead", DcType::Float,  8,   0,  30,  true  },

    // Patrol-wait (Dynamic mode only). A human tank times pulls around patrols. When
    // the ONLY thing pushing a pack's aggro estimate over the Leeroy ceiling is a
    // lone DB-authored patroller in chain range (the estimate without it is a clean
    // small Leeroy), the tank holds at commit range and waits the patrol out instead
    // of committing the heavier Advanced maneuver, then Leeroys once it passes.
    // PullPatrolWait is the master toggle. PullPatrolWaitSec is the max hold before
    // it gives up and proceeds with the Advanced verdict (a stationary / very slow
    // patrol mustn't stall the run). See DungeonClearMath::ShouldWaitForPatrol +
    // DcPullPlanner::UpdateDynamicPullMode (pull decision == 3 = waiting-for-patrol).
    { "PullPatrolWait",        DcType::Bool,   1,   0,   1,  true  },
    { "PullPatrolWaitSec",     DcType::Float,  8,   1,  30,  true  },

    // Liquid avoidance. The route producers include water/magma polys so the
    // bot CAN swim/wade when there is no dry alternative, but with these per-area
    // Detour cost multipliers a crossing only wins when it is genuinely shorter:
    // an all-land detour up to WaterPathCost times longer than the water shortcut
    // is preferred. 1.0 = no preference (water as cheap as land). MagmaPathCost is
    // set high so lava is shunned but still traversable as an absolute last
    // resort (the player nav-filter already excludes slime outright). These feed
    // dtQueryFilter::setAreaCost in LongRangePathfinder + CorridorCenter; both run
    // off the map thread, so they are server-only (read straight from conf, never
    // the per-run override store). See DungeonClearGeometry::ApplyLiquidAreaCosts.
    { "WaterPathCost",         DcType::Float,  3,   1,  50,  false },
    { "MagmaPathCost",         DcType::Float, 20,   1, 1000, false },

    // Submerged swim legs (Tier A). When the navmesh route to a target dead-ends
    // AND water lies between, the bot greedily 3D-swims to it instead of stalling
    // (the navmesh has no mesh under liquid — only a surface sheet — so a
    // submerged tunnel is otherwise unreachable). SwimMaxRange bounds how far a
    // dead-end target may be before a swim is attempted (caps the greedy search
    // and avoids trying to swim to something genuinely out of reach).
    { "SwimEnable",            DcType::Bool,   1,   0,    1,  false },
    { "SwimMaxRange",          DcType::Float, 250, 30, 1000,  false },

    // Spectator free-camera (`.dc spectate`). SpectateEnable is the admin gate:
    // the free-fly camera detaches the player from their body (the bots keep
    // playing it), which some servers consider a cheat, so it is server-only and
    // can be switched off entirely — DcSpectator::Start refuses with a message
    // when it is 0. SpectateSpeed is the movement speed multiplier applied to the
    // possessed camera dummy (flight and run). See Util/DcSpectator.h.
    { "SpectateEnable",        DcType::Bool,   1,   0,   1,  false },
    { "SpectateSpeed",         DcType::Float, 2.5, 0.5,  8,  true  },

    // Test-run harness (`.dc test`). Server-only: these govern the regression
    // harness, never a live dungeon run, so the addon neither shows nor
    // overrides them. They live here (rather than being read straight from
    // sConfigMgr at the call site) for the same reason every other tunable does
    // — DcSettings reads with showLogs=false and caches, and the concurrency
    // caps in particular are re-read on EVERY world tick by DcTestRunManager::
    // Tick / DcTestPlanManager::TickPlan. A raw GetOption there floods the
    // console with "Config: Missing property ..." whenever the deployed conf
    // predates the key (thousands of lines per session). See DcSettings.h.
    // MaxConcurrent / MaxPlans / Plan.MaxTotal all take 0 = unlimited, hence the
    // 0 floor — and all three DEFAULT to 0. The harness deliberately imposes no
    // ceiling of its own: how many runs the box can field is a property of the
    // box (AiPlayerbot.MaxAddedBots, the addclass pool, CPU), and those limits
    // already refuse an over-budget start with a named message. A second,
    // harness-local cap only ever refused starts the machine could have served.
    { "TestRun.MaxConcurrent",   DcType::UInt,      0,  0, 100000, false },
    { "TestRun.MaxPlans",        DcType::UInt,      0,  0, 100000, false },
    { "TestRun.PauseGraceS",     DcType::UInt,     60,  0,   3600, false },
    { "TestRun.StallGraceS",     DcType::UInt,    120,  0,   3600, false },
    { "TestRun.NoProgressS",     DcType::UInt,    600,  0,  86400, false },
    { "TestRun.OverallTimeoutS", DcType::UInt,   3600, 60,  86400, false },
    { "TestRun.Plan.MaxTotal",   DcType::UInt,      0,  0, 100000, false },
    { "TestRun.Plan.BackoffMs",  DcType::UInt,   5000,  0, 600000, false },
    { "TestRun.Plan.DriverWaitMs", DcType::UInt, 120000, 0, 600000, false },

    // Server-only (not overridable from the addon).
    { "AsyncPathfinding",      DcType::Bool,   1,   0,   1,  false },
    { "PathCenterEnable",      DcType::Bool,   1,   0,   1,  false },
    { "PathWallClearance",     DcType::Float,  3,   0,  10,  false },
    { "PathCenterMaxPush",     DcType::Float,  5,   0,  10,  false },
    { "PathCenterSmoothIters", DcType::Int,    2,   0,   8,  false },
};

inline constexpr std::size_t kDcSettingCount =
    sizeof(kDcSettings) / sizeof(kDcSettings[0]);

// Linear lookup by key suffix; nullptr if the key is not registered. The table
// is tiny, so a scan is cheaper than any map and keeps it constexpr-friendly.
inline DcSettingDef const* FindDcSetting(std::string_view key)
{
    for (DcSettingDef const& d : kDcSettings)
        if (key == d.key)
            return &d;
    return nullptr;
}

#endif
