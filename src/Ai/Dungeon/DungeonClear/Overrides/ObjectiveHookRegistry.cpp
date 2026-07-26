/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ObjectiveHookRegistry.h"

#include <atomic>
#include <cmath>
#include <list>
#include <string>
#include <unordered_map>

#include "Creature.h"
#include "CreatureAI.h"
#include "GameObject.h"
#include "Group.h"
#include "InstanceScript.h"
#include "Item.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "ServerFacade.h"
#include "Player.h"
#include "Spell.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Data/Events/DungeonEventTables.h"
#include "Ai/Dungeon/DungeonClear/Util/DcFormGate.h"
#include "Ai/Dungeon/DungeonClear/Util/DcMovement.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonPathFollower.h"
#include "Ai/Dungeon/DungeonClear/Util/LongRangePathfinder.h"

namespace
{
    // --- Blackrock Depths Ring of Law: EnsureRingStarted (hook id 1) -------
    // The Ring of Law starts when a party member crosses area trigger 1526 at
    // the arena centre. Normally that's the human (or a self-bot relayed from
    // the master). But a bot never autonomously sends CMSG_AREATRIGGER, so an
    // all-bot party (or a human not on the trigger when the tank arrives) would
    // never start it. Used as the Ring of Law event's second step (a Custom
    // step), this fires the REAL trigger from the leader once it's standing on
    // the spot: the core validates the bot is within the trigger radius and runs
    // at_ring_of_law for real (which itself honours the 2-minute post-wipe
    // cooldown and no-ops if already started). Done once the encounter is at
    // least IN_PROGRESS; Running (and re-fires, harmlessly idempotent) until then.
    constexpr uint32 BRD_TYPE_RING_OF_LAW = 1;  // DataTypes::TYPE_RING_OF_LAW
    constexpr uint32 BRD_RING_IN_PROGRESS = 1;  // EncounterState::IN_PROGRESS
    constexpr uint32 BRD_RING_OF_LAW_TRIGGER = 1526;

    ObjectiveArriveResult EnsureRingStarted(Player* bot, AiObjectContext* /*context*/,
                                            DungeonBossInfo const& /*info*/)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;  // not in the instance yet

        // Already running (or DONE) — let the hold step take over.
        if (inst->GetData(BRD_TYPE_RING_OF_LAW) >= BRD_RING_IN_PROGRESS)
            return ObjectiveArriveResult::Done;

        // Fire the real area trigger from the leader. Same supported path
        // ReachAreaTriggerAction uses for a non-teleport trigger; the core
        // range-checks and runs the at_ring_of_law script.
        WorldPacket p(CMSG_AREATRIGGER);
        p << uint32(BRD_RING_OF_LAW_TRIGGER);
        p.rpos(0);
        bot->GetSession()->HandleAreaTriggerOpcode(p);
        return ObjectiveArriveResult::Running;
    }

    // --- Deadmines Defias Cannon: FireDefiasCannon (hook id 2) ------------
    // The way to the pirate ship is sealed by the Iron Clad Door (GO 16397,
    // lock 202 — rogue-pick only, so a bot party can never click it open). A
    // player loots the Defias Gunpowder chest, then USES the gunpowder on the
    // Defias Cannon (GO 16398): that casts spell 6250 at the cannon, whose
    // SmartGameObjectAI (SMART_EVENT_SPELLHIT on 6250) fires the cannon —
    // opening the door to GO_STATE_ACTIVE_ALTERNATIVE (the GOState enum's
    // "closed door open by cannon fire"), summoning two Defias Squallshapers,
    // and setting the cannon instance data.
    //
    // Spell 6250 is SPELL_EFFECT_OPEN_LOCK against the cannon, and the cannon's
    // lock (83) is an ITEM lock requiring the Defias Gunpowder (item 5397) as
    // the key — so the spell only HITS the cannon (and fires the SmartAI) when
    // it is cast FROM that item, supplying the cast-item the lock check needs.
    // A bare CastSpell(6250) with no cast item fails the lock and never hits.
    //
    // A bot never loots the chest, so grant it the gunpowder and use the item on
    // the cannon via the full item-use path. Used as the Deadmines event's
    // Custom step (DeadminesEvents). The gunpowder has a 2s "Opening" cast, so:
    //  - if the door is already open, the cannon has fired -> Done (idempotent;
    //    also covers a re-init after an earlier attempt left the door open, where
    //    a second fire would summon two more Squallshapers);
    //  - if the opening cast is already in flight, let it finish (don't re-cast
    //    and interrupt ourselves every tick);
    //  - otherwise grant + use the gunpowder. Returns Running; the event's
    //    door-state gate is what confirms the open and advances.
    constexpr uint32 DM_GO_CANNON         = 16398;
    constexpr uint32 DM_GO_IRON_CLAD_DOOR = 16397;
    constexpr uint32 DM_ITEM_GUNPOWDER    = 5397;  // casts OPEN_LOCK spell 6250

    ObjectiveArriveResult FireDefiasCannon(Player* bot, AiObjectContext* /*context*/,
                                           DungeonBossInfo const& /*info*/)
    {
        // Door already open (state != READY) -> the cannon has fired.
        if (GameObject* door = bot->FindNearestGameObject(DM_GO_IRON_CLAD_DOOR, 100.0f))
            if (door->GetGoState() != GO_STATE_READY)
                return ObjectiveArriveResult::Done;

        // The 2s gunpowder "Opening" cast is already running — let it complete.
        if (bot->IsNonMeleeSpellCast(false))
            return ObjectiveArriveResult::Running;

        GameObject* cannon = bot->FindNearestGameObject(DM_GO_CANNON, 40.0f);
        if (!cannon)
            return ObjectiveArriveResult::Running;  // not in range yet

        // Grant the gunpowder (bots never looted the chest) and use it on the
        // cannon: the item is the OPEN_LOCK key, so the spell hits and the
        // cannon's SmartAI fires.
        Item* gunpowder = bot->GetItemByEntry(DM_ITEM_GUNPOWDER);
        if (!gunpowder)
        {
            bot->AddItem(DM_ITEM_GUNPOWDER, 1);
            gunpowder = bot->GetItemByEntry(DM_ITEM_GUNPOWDER);
            if (!gunpowder)
                return ObjectiveArriveResult::Running;  // bags full this tick
        }

        // Feral-form druids can't cast the item spell at all — shift back first
        // (same gate the barrel plants use). See DcFormGate.
        DcFormGate::DropBlockingForm(bot, gunpowder);
        SpellCastTargets targets;
        targets.SetGOTarget(cannon);
        bot->CastItemUseSpell(gunpowder, targets, 0, 0);
        return ObjectiveArriveResult::Running;  // door-state gate confirms it
    }

    // --- ZulFarrak: WakeZumrah (hook id 5) --------------------------------
    // Witch Doctor Zum'rah (7271) spawns with creature_template faction 35
    // (friendly to everyone) — he is NOT flag-gated, so nothing in the engage
    // path reads him as un-engageable. The ONLY thing that turns him hostile is
    // area trigger 962 (map 209, centre 1909.27/1015.11/11.5155, radius 10 —
    // roughly 3yd off his spawn), whose SmartAI row sets his spawn's faction to
    // 37:
    //   smart_scripts (962, source_type 2, event 46 ON_TRIGGER) -> SET_FACTION 37
    //
    // That row is reachable only from CMSG_AREATRIGGER, which a bot never sends
    // on its own: mod-playerbots either mirrors the human master's own crossing
    // (PlayerbotAI's "area trigger" master handler) or fires autonomously but
    // ONLY for teleport triggers (WithinAreaTrigger / ReachAreaTriggerAction both
    // bail on !GetAreaTriggerTeleport). AT 962 has no areatrigger_teleport row,
    // so an all-bot party — every `dc test` run — walks up to a permanently
    // friendly Zum'rah. The engage gate then loops forever against a live but
    // unattackable boss and the run deadlocks there.
    //
    // The first attempt at this forged CMSG_AREATRIGGER from the leader (the way
    // EnsureRingStarted starts the Ring of Law) so the genuine SmartAI row would
    // run. It did NOT work in a live all-bot run — the tank parked on the trigger
    // and Zum'rah stayed friendly — and no log survived to say which stage failed
    // (condition, core range check, or SmartAI target resolution). Rather than
    // spend build cycles bisecting a path with three opaque failure modes, set
    // the faction the SmartAI row would have set. The row's ONLY action is
    // SET_FACTION 37 on this spawn, so the end state is identical; what we give
    // up is going through the script.
    //
    // Faction 37 (not "hostile" generically) is exactly what the row applies. His
    // own "On Reset - Restore faction" row puts him back to 35 after a wipe, and
    // the repeatable event re-fires — so the reset path still works. Setting the
    // faction is enough on its own: once hostile his normal aggro takes over and
    // he pulls the party, no engage nudge required.
    //
    // Logged at INFO because this is the one place a ZF deadlock gets fixed
    // silently; under the test harness a bot-side failure is otherwise invisible
    // (see the TellError lesson in the test-plan notes).
    constexpr uint32 ZF_ZUMRAH = 7271;
    constexpr uint32 ZF_ZUMRAH_FACTION_FRIENDLY = 35;  // creature_template default
    constexpr uint32 ZF_ZUMRAH_FACTION_HOSTILE  = 37;  // what AT 962's row sets
    // Wide enough to still find him from the trigger spot if he has wandered;
    // the party is standing on top of him by the time this step runs.
    constexpr float ZF_ZUMRAH_SCAN = 60.0f;

    ObjectiveArriveResult WakeZumrah(Player* bot, AiObjectContext* /*context*/,
                                     DungeonBossInfo const& /*info*/)
    {
        Creature* zumrah = bot->FindNearestCreature(ZF_ZUMRAH, ZF_ZUMRAH_SCAN);
        if (!zumrah || !zumrah->IsAlive())
            return ObjectiveArriveResult::Done;  // gone/dead — nothing to wake

        // Already flipped (this fired, or a human crossed the trigger).
        if (zumrah->GetFaction() != ZF_ZUMRAH_FACTION_FRIENDLY)
            return ObjectiveArriveResult::Done;

        zumrah->SetFaction(ZF_ZUMRAH_FACTION_HOSTILE);
        LOG_INFO("playerbots.dungeonclear",
                 "DungeonClear: ZulFarrak — set Zum'rah ({}) faction {} -> {} for {} "
                 "(area trigger 962 never fires for an all-bot party)",
                 ZF_ZUMRAH, ZF_ZUMRAH_FACTION_FRIENDLY, ZF_ZUMRAH_FACTION_HOSTILE,
                 bot->GetName());
        return ObjectiveArriveResult::Done;
    }

    // --- The Arcatraz: DriveMellicharWaves (hook id 6) --------------------
    // Warden Mellichar's stasis-pod finale is started by DAMAGING him — there is
    // no gossip, no area trigger and no clickable object anywhere in the
    // encounter (arcatraz.cpp, npc_warden_mellicharAI::DamageTaken). He is
    // SetImmuneToAll and zeroes all incoming damage, so the hit costs him
    // nothing; it exists purely as the signal that schedules the intro EventMap.
    // He never enters combat and never loses HP, so a KillCreatureEngage step
    // aimed at him would run forever — hence a Custom step.
    //
    // DO NOT "simplify" THIS TO SetBossState(DATA_WARDEN_MELLICHAR, IN_PROGRESS).
    // The intro events are scheduled inside DamageTaken, NOT inside SetBossState,
    // so setting the state directly starts nothing — AND it permanently locks the
    // encounter out, because DamageTaken's own guard is
    // `GetBossState(...) != IN_PROGRESS`. That is a one-way trap: the run can
    // never be recovered without a full instance reset.
    //
    // THIS HOOK OWNS THE WHOLE WAVE PHASE (poke + garrison), rather than poking
    // once and handing off to a MoveToHoldUntilSpawn step, SPECIFICALLY so a wipe
    // recovers. The event must be .Persistent() (it holds a KillCreatureEngage),
    // and DungeonEventExecutor::Drive only rewinds stepIndex for NON-persistent
    // events — so a poke-once step that has already returned Done can never fire
    // again. A wipe mid-waves puts Mellichar's own Reset() back to NOT_STARTED
    // and despawns every summon, and a separate hold step would then sit waiting
    // on a Skyriss that nobody will ever release, burning the full step timeout.
    // Keeping the poke live for the entire phase makes that self-healing: the
    // party corpse-runs back, this sees NOT_STARTED again, and re-pokes.
    constexpr uint32 ARC_MELLICHAR = 20904;
    constexpr uint32 ARC_SKYRISS = 20912;
    constexpr uint32 ARC_DATA_WARDEN_MELLICHAR = 3;  // arcatraz.h DATA_WARDEN_MELLICHAR
    constexpr uint32 ARC_NOT_STARTED = 0;            // EncounterState::NOT_STARTED
    constexpr uint32 ARC_DONE = 3;                   // EncounterState::DONE
    // He is a fixed spawn on the dais ~8yd from the arena centroid the event
    // parks on; 60 covers the whole pod room without reaching outside it.
    constexpr float ARC_MELLICHAR_SCAN = 60.0f;
    // Skyriss is a TempSummon, so only a grid scan finds him; he is released
    // ~30yd from the centroid.
    constexpr float ARC_SKYRISS_SCAN = 100.0f;
    // The arena floor centroid, mirrored from ArcatrazEvents.cpp. Re-centring
    // matters because the encounter evades outright if no player is within 100yd
    // of Mellichar (EVENT_WARDEN_CHECK_PLAYERS, every 1s).
    constexpr float ARC_ARENA_X = 445.9f;
    constexpr float ARC_ARENA_Y = -161.5f;
    constexpr float ARC_ARENA_Z = 42.56f;
    constexpr float ARC_ARENA_LEASH = 15.0f;

    ObjectiveArriveResult DriveMellicharWaves(Player* bot, AiObjectContext* /*context*/,
                                              DungeonBossInfo const& /*info*/)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;  // not in the instance yet

        // Skyriss is on the field — hand off to the kill step.
        if (bot->FindNearestCreature(ARC_SKYRISS, ARC_SKYRISS_SCAN, /*alive*/ true))
            return ObjectiveArriveResult::Done;

        // Already won (a resumed run, or the human finished it) — don't restart it.
        if (inst->GetBossState(ARC_DATA_WARDEN_MELLICHAR) == ARC_DONE)
            return ObjectiveArriveResult::Done;

        if (inst->GetBossState(ARC_DATA_WARDEN_MELLICHAR) == ARC_NOT_STARTED)
        {
            Creature* mellichar = bot->FindNearestCreature(ARC_MELLICHAR, ARC_MELLICHAR_SCAN);
            if (mellichar && mellichar->IsAlive())
            {
                // Any nonzero player-sourced damage will do; DamageTaken checks
                // only `attacker->GetCharmerOrOwnerOrOwnGUID().IsPlayer() &&
                // damage > 0`. Unit::DealDamage hands the AI its DamageTaken
                // callback BEFORE any HP arithmetic, and Mellichar's override
                // zeroes the value, so this cannot overkill him or otherwise
                // perturb the script.
                Unit::DealDamage(bot, mellichar, 1);
                // Throttled: if the poke ever fails to take (another module's
                // DealDamage hook zeroing damage ahead of the script, say) this
                // retries every tick, and an unthrottled INFO would bury the log.
                static uint32 lastPokeLog = 0;
                uint32 const now = getMSTime();
                if (!lastPokeLog || GetMSTimeDiffToNow(lastPokeLog) > 10000)
                {
                    lastPokeLog = now;
                    LOG_INFO("playerbots.dungeonclear",
                             "DungeonClear: Arcatraz — poking Warden Mellichar ({}) to open the "
                             "stasis pods for {} (the encounter has no trigger but player damage)",
                             ARC_MELLICHAR, bot->GetName());
                }
            }
        }

        // Garrison the centroid between waves. The executor's own MoveTo
        // re-centring is unavailable here (this is one Custom step, deliberately
        // — see above), so mirror it: only nudge when actually displaced and not
        // already moving, so this never fights the combat AI mid-wave.
        if (!bot->isMoving() &&
            bot->GetExactDist(ARC_ARENA_X, ARC_ARENA_Y, ARC_ARENA_Z) > ARC_ARENA_LEASH)
        {
            bot->GetMotionMaster()->MovePoint(0, ARC_ARENA_X, ARC_ARENA_Y, ARC_ARENA_Z,
                                              FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                              /*generatePath*/ true, false);
        }

        return ObjectiveArriveResult::Running;
    }

    // --- Sethekk Halls: DriveAnzuSummon (hook id 7) -----------------------
    // Anzu is normally summoned by a DRUID using item 32449 (which sends event
    // 14797 to the instance). instance_sethekk_halls::ProcessEvent has NO
    // IsHeroic()/difficulty gate — "heroic-only" is enforced purely by the
    // item/quest being heroic-gated — so we replicate the send-event directly (no
    // druid / item / quest / heroic key). The blizzlike HEROIC-only restriction is
    // reinstated one level up: the Anzu event carries .HeroicOnly(), so this hook
    // is only ever driven on a heroic run. On normal the event never fires.
    // The instance guard self-de-dupes (no Voice && Anzu != DONE), but we also
    // gate our own poke on "no Anzu yet" so we never spawn a SECOND Voice/Anzu
    // once he is on the field. ~40s of theatrics then a ~16s intro precede a
    // fightable Anzu; the Custom step's 120s Timeout covers it, and Anzu's own
    // SetInCombatWithZone() (post-intro) pulls the party into the fight.
    constexpr uint32 SH_ANZU_SEND_EVENT = 14797;
    constexpr uint32 SH_ANZU_NPC        = 23035;  // TempSummon boss
    constexpr uint32 SH_VOICE_NPC       = 21851;  // TempSummon theatrics NPC
    constexpr uint32 SH_DATA_ANZU       = 1;      // sethekk_halls.h DATA_ANZU
    constexpr float  SH_ANZU_SCAN       = 60.0f;

    ObjectiveArriveResult DriveAnzuSummon(Player* bot, AiObjectContext* /*context*/,
                                          DungeonBossInfo const& /*info*/)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;  // not in the instance yet

        // Already killed (a resumed run, or the human did it) — don't re-summon.
        if (inst->GetBossState(SH_DATA_ANZU) == DONE)
            return ObjectiveArriveResult::Done;

        // Anzu is on the field (even mid-intro, still NON_ATTACKABLE) — hand off
        // to the KillCreatureEngage step and STOP poking so no second Voice fires.
        if (bot->FindNearestCreature(SH_ANZU_NPC, SH_ANZU_SCAN, /*alive*/ true))
            return ObjectiveArriveResult::Done;

        // Theatrics in progress — the Voice is alive running action list 2185100
        // toward the ~40s summon. Hold without re-poking.
        if (bot->FindNearestCreature(SH_VOICE_NPC, SH_ANZU_SCAN, /*alive*/ true))
            return ObjectiveArriveResult::Running;

        // No Voice, no Anzu, not DONE -> fire the send-event. Idempotent (the
        // instance guard no-ops if a Voice somehow already exists).
        inst->ProcessEvent(bot, SH_ANZU_SEND_EVENT);
        return ObjectiveArriveResult::Running;
    }

    // === The Black Morass (map 269) — CoT: Opening of the Dark Portal ======
    //
    // THE ENCOUNTER, from the core scripts (instance_the_black_morass.cpp +
    // the_black_morass.cpp), because every rule below falls out of it:
    //
    //  * A Time Rift (17838) opens. +6s it summons its KEEPER (a Rift Lord /
    //    Rift Keeper, or on waves 6/12/18 the bosses Deja / Temporus / Aeonus).
    //    +16s, and every 15s after that FOREVER, it summons one trash add.
    //  * Every add is spawned REACT_DEFENSIVE and sent (DoSummonAtRift ->
    //    SetHomePosition + MoveTargetedHome) to a home 14yd from Medivh, where it
    //    RESETS and casts Corrupt on itself; the aura ticks SetData(
    //    DATA_DAMAGE_SHIELD) and Medivh's 100% shield falls. 0% loses the run.
    //  * npc_time_rift::SummonedCreatureDies despawns the rift the instant its
    //    keeper dies. KILLING THE KEEPER IS THE ONLY SHUTOFF FOR THE ADD PUMP.
    //  * Close the only open rift and the next opens in 1s; leave it open and the
    //    next opens anyway at 90s (2min from rift 13), up to 4 concurrent.
    //
    // So the whole encounter reduces to one quantity — open rifts x 15s = adds
    // produced — and to one strategy, which is exactly how a human party plays it:
    //
    //      hold ON Medivh (every add walks to him, so it is the one spot that
    //      intercepts all four rifts with zero travel) -> when Medivh's ring is
    //      clean, run to the open rift and kill its KEEPER, picking up the adds
    //      that spawn on top of you -> the rift closes -> run back and clean up
    //      whatever leaked -> next rift.
    //
    // HISTORY — why this is one decision hook and not a step list. The previous
    // shape was a 17-step sequential event (camp -> nine per-entry drainer kills
    // -> four portal volumes -> Medivh -> arena) fronted by an arena-wide
    // force-pull. Three failures were structural to that shape, and all three are
    // the reason a run "fell apart once two portals were open":
    //
    //  1. The whole thing was driven only OUT OF COMBAT (the conditional-event
    //     rung stands down on bot->IsInCombat()). This is a continuous wave fight;
    //     with two rifts pumping, combat never dropped, the driver never ran, and
    //     the tank simply stood where its last fight ended. Fixed at the framework
    //     — the wave event is flagged DrivesInCombat() and a combat-engine copy of
    //     the rung (DcRel::EventDueCombat) drives it under fire.
    //  2. The keeper — the only shutoff — sat BEHIND nine drainer-kill gates that
    //     a live rift re-blocked every 15s, so it was never reached and no rift
    //     ever closed. A sequential step list cannot express "always prefer the
    //     keeper"; it can only express "do these in order and block".
    //  3. The arena-wide force-pull ran FIRST, so it dragged the whole arena into
    //     combat and then the (non-combat) driver went dormant — a combat-lock
    //     generator in front of the only useful work.
    //
    // The force-pull ITSELF was a correct discovery and is kept below: the adds
    // are REACT_DEFENSIVE and parked in a reset/home idle, so they never aggro the
    // party and DcTargeting's selector (via AttackersValue::IsPossibleTarget)
    // cannot see them either — the party stands on top of a mob draining the
    // shield and does nothing. It is now a SIDE EFFECT scoped to what the party is
    // already next to, and it never gates and never redirects the tank.

    // --- instance data / entries ------------------------------------------
    constexpr uint32 BM_DATA_AEONUS         = 2;   // the_black_morass.h DATA_AEONUS
    // GetData: Medivh's shield 100..0. 0 == the event has FAILED and he is dead.
    constexpr uint32 BM_DATA_SHIELD_PERCENT = 13;  // the_black_morass.h DATA_SHIELD_PERCENT
    constexpr uint32 BM_DATA_RIFT_NUMBER    = 14;  // GetData: current rift 0..18
    constexpr uint32 BM_NPC_MEDIVH          = 15608;
    constexpr uint32 BM_NPC_TIME_RIFT       = 17838;
    // The wave-18 boss. Filed as a DRAINER, not a keeper (see kBmDrainEntries in
    // BlackMorassEvents.cpp), so nothing in the rift machinery below will ever
    // find him — the wave driver has to name him explicitly.
    constexpr uint32 BM_NPC_AEONUS          = 17881;  // the_black_morass.h NPC_AEONUS

    // --- geometry ----------------------------------------------------------
    // Stand point -> Medivh is a few yards; 100 finds him from anywhere the
    // garrison roams without reaching outside the arena.
    constexpr float BM_MEDIVH_SCAN = 100.0f;

    // THE HOLD POINT — 12yd from Medivh's spawn, on the bearing toward the arena
    // centre. Mirrored in BlackMorassEvents.cpp (the objective anchor); the two
    // must agree or the objective garrison and the wave driver fight over the tank.
    //
    // It used to be 38yd out, deliberately OUTSIDE Medivh's 20yd start trigger.
    // That was backwards on both counts. The adds home to a ring 14yd around
    // MEDIVH, so his ring is the one place in the arena that intercepts all four
    // rifts for free — standing 38yd away means every add gets to establish its
    // channel before anyone reaches it. And the trigger is not something to avoid:
    // by the time the party is at this objective the grounds are pre-cleared and
    // STARTING the event is the whole point (npc_medivh_bm::MoveInLineOfSight
    // guards itself with `!events.Empty()`, so arriving again later is a no-op).
    constexpr float BM_HOLD_X = -2014.4f, BM_HOLD_Y = 7114.6f, BM_HOLD_Z = 22.8f;
    constexpr float BM_GARRISON_LEASH = 12.0f;

    // --- the start walk-in -------------------------------------------------
    // The event starts on npc_medivh_bm::MoveInLineOfSight, whose gate is
    // `me->IsWithinDistInMap(who, 20.0f)` — a 3D distance to MEDIVH, not to any
    // fixed spot. An earlier cut aimed at a hand-picked point and reused the 12yd
    // garrison leash as the "close enough" gate, so the tank stopped pushing as
    // far as 28yd from Medivh and the trigger never fired (live 2026-07-24: parked
    // at 25.9yd for 4+ minutes with the rift counter stuck at 0). So: aim at
    // MEDIVH HIMSELF (a near-point on the bot's side of him, so the walk-in
    // resolves real terrain height — the tank stands ~3.5yd below his Z on the
    // swamp floor, which the 3D check charges against the 20yd budget) and keep
    // pushing until genuinely inside with margin.
    constexpr float BM_START_APPROACH = 8.0f;   // walk-in target: 8yd from Medivh
    constexpr float BM_START_HOLD     = 14.0f;  // stop pushing only inside this (< 20)

    // Beyond this a bare MovePoint is not trusted to deliver. The engine
    // PathGenerator caps a generated path at 74 polys / 74 points, so a
    // cross-arena request truncates — and it does so SILENTLY: the bot simply
    // stands still, with no failure to observe at the call site.
    constexpr float BM_LONG_HAUL = 30.0f;

    // Camp leash. Adds spawn <=10yd from the rift and walk PAST an idle party to
    // Medivh unless pulled, so "camped" has to mean close enough to grab them on
    // the pop — tighter than the garrison leash.
    constexpr float BM_CAMP_LEASH = 8.0f;

    // "Is the glide in flight still aimed at the right place?" — deliberately NOT
    // the arrival leash, which is far tighter. Some destinations are re-derived
    // every tick and drift a few yards (a live keeper is a moving target; the
    // Medivh walk-in aims at a near-point that slides around him as the approach
    // angle changes), and comparing those against a 3-8yd leash would cancel and
    // re-path the glide every tick. What we DO need to tell apart are the four
    // portals, 80-102yd from the hold point and from each other, so a coarse
    // epsilon separates "same destination, jittered" from "a different portal".
    constexpr float BM_REPATH_EPSILON = 15.0f;

    // Floor between two spline issues for the SAME destination.
    //
    // Out of combat the escort-generator check in BmTravelTo is enough on its own:
    // once the glide is issued, GetCurrentMovementGeneratorType() reads ESCORT and
    // every later tick returns early. IN COMBAT it is not — the stock combat
    // engine layers MoveChase back over the escort slot whenever it wins a tick,
    // so the type check stops being able to tell "glide in flight" from "nothing
    // issued", and without a time floor the driver would rebuild and re-issue a
    // cross-arena route every single tick. 1.5s bounds that to a handful of
    // re-issues per 100yd haul while still recovering promptly when the chase
    // really did stomp our spline.
    constexpr uint32 BM_REISSUE_MS = 1500;

    // Portals sit 80-102yd from the hold point; 250 sees the whole arena (and
    // matches the wave event's activation predicate, so the gate and the driver
    // can never disagree about whether a rift is up).
    constexpr float BM_RIFT_SCAN = 250.0f;

    // A keeper is summoned <=10yd from its rift (npc_time_rift GetNearPosition(10))
    // and, unlike the adds, is a normal aggressive mob that closes on the party.
    // 40 still recognises it as "this rift's keeper" once the fight has dragged it
    // around, without reaching the next portal (the closest pair is ~70yd apart).
    constexpr float BM_KEEPER_BAND = 40.0f;

    // Once the tank is this close to the keeper, FINISH IT — the sweep diversion
    // below stands down. Abandoning a keeper we have already crossed the arena for
    // is strictly worse than letting a drainer tick a few more seconds: the keeper
    // is the pump, and it dies in seconds once engaged.
    constexpr float BM_KEEPER_COMMIT = 50.0f;

    // Force-pull radius, measured from the BOT. Deliberately NOT arena-wide.
    //
    // The pull's whole job is to hand the combat engine a mob it would otherwise
    // ignore — one standing right next to the party in a reset idle. Pull one from
    // across the arena instead and it runs at the tank, loses us (or the tank
    // walks on to the next rift), and npc_black_morass_summoned_add::
    // EnterEvadeMode sends it straight back home to resume channelling — so the
    // wide pull buys nothing and costs a party dragged into a fight it did not
    // choose. 60yd is "everything we are standing among": from anywhere inside the
    // Medivh delivery band it covers his whole 14yd add ring including the far
    // side, and from a camped portal it covers every fresh spawn (<=10yd) while
    // still falling well short of Medivh (94-138yd from any portal), so a camped
    // party never yanks his drainers across the arena.
    constexpr float BM_PULL_RADIUS = 60.0f;

    // The ring around MEDIVH that counts as "draining". Adds home at exactly 14yd;
    // 30 also counts one on the last steps of its walk in, which is about to drain
    // and is worth the same trip.
    constexpr float BM_MEDIVH_RING = 30.0f;

    // --- DELIVERY BANDS: where the driver stops steering ------------------
    // The driver's job is CROSS-ARENA delivery, not the last few yards. Inside
    // these bands it stands down and hands the tick back, and the stock combat
    // engine's MoveChase closes the remaining distance while the tank fights.
    //
    // Two separate failures made this necessary, both of them the same mistake in
    // different clothing — the driver claiming ticks it did not need:
    //
    //  * Engine::DoNextAction runs exactly ONE action per tick. A driver that
    //    reports Running every tick therefore starves every lower rung, and this
    //    one sits above the stock combat movers in the combat engine. Live: the
    //    tank drove to the portal, the keeper engaged it, and the tank then stood
    //    there taking hits with NO rotation and NO threat while the DPS pulled
    //    aggro and the party wiped.
    //  * A tight arrival leash re-triggers travel over a couple of yards, so a
    //    tank in melee (dist ~5yd) with an 8yd leash would break off, walk 2yd,
    //    and re-enter combat, over and over.
    //
    // So the bands are deliberately loose. 25yd is comfortably inside the party's
    // engage range and well within what MoveChase closes in a second or two.
    constexpr float BM_DELIVERED_KEEPER = 25.0f;
    constexpr float BM_DELIVERED_MEDIVH = 20.0f;

    // No constant for the wave driver's hook id here: it is consumed only by the
    // Hooks() table below (as a literal) and by BlackMorassEvents.cpp (which
    // builds the step), so a copy in this file would be dead. That matches hooks
    // 9 and 10, which name their id in the section heading and nowhere else.

    // PER-BOT throttled travel log.
    //
    // The camp line's throttle was a single global static, so with 8-10 concurrent
    // instances every consecutive line came from a DIFFERENT bot — which makes the
    // one measurement that matters ("is this bot's distance falling tick over
    // tick?") impossible to read. thread_local keyed by GUID gives a genuine
    // per-bot window and is race-free without a lock: a map's bots are updated on
    // one map-update thread, so each thread owns its table.
    void BmTravelLog(Player* bot, char const* what, float dist, std::string const& detail)
    {
        thread_local std::unordered_map<uint32, uint32> lastMs;
        uint32 const guid = bot->GetGUID().GetCounter();
        uint32 const now = getMSTime();
        uint32& prev = lastMs[guid];
        if (prev && GetMSTimeDiffToNow(prev) < 3000)
            return;
        prev = now;
        LOG_DEBUG("playerbots.dungeonclear",
                 "DungeonClear: Black Morass — {} travel: {} (dist {:.1f}yd){}{}",
                 bot->GetName(), what, dist, detail.empty() ? "" : " — ", detail);
    }

    // Travel to an arena point, long-haul aware and safe to call every tick while
    // IN COMBAT.
    //
    // EVERY Black Morass destination is a long haul: the four portals sit 80-102yd
    // from the hold point and the arena is ~150yd across, all of it open swamp
    // with no intervening room to chunk against. A bare MovePoint cannot deliver
    // that range and fails SILENTLY. Live evidence from a 10-run batch
    // (2026-07-24): 151 rift-camp attempts, NONE arrived within 20yd, 94 were
    // still 100+yd out. With the party never reaching the portal, every wave
    // walked into Medivh unopposed. So: keep the cheap MovePoint for a genuine
    // short hop, and route anything longer through LongRangePathfinder — the
    // module's whole-route Detour A*, the same producer the advance glide uses —
    // issued as ONE escort spline.
    //
    // NOTE what is deliberately NOT here any more: the old `else if
    // (bot->isMoving()) return;` guard. It was written to keep the garrison from
    // fighting the combat engine, and it did — by making the driver a no-op for
    // the entire encounter, since in combat the bot is essentially always moving
    // under MoveChase. This hook now owns the tick (DcRel::EventDueCombat, above
    // the stock combat movers) precisely so it CAN take the bot off its current
    // target and walk it to the next rift; BM_REISSUE_MS is what keeps that from
    // becoming spline spam.
    void BmTravelTo(Player* bot, float x, float y, float z, float leash)
    {
        if (!bot)
            return;

        float const dist = bot->GetExactDist(x, y, z);
        if (dist <= leash)
            return;  // arrived

        // An escort glide already in flight owns the bot. Let it finish if it is
        // headed here; drop it if it is stale — a route to the PREVIOUS portal
        // (closed since we launched) would otherwise ride all the way out before
        // the bot could react to the rift that is actually open.
        MotionMaster* mm = bot->GetMotionMaster();
        float dx, dy, dz;
        if (mm && mm->GetCurrentMovementGeneratorType() == ESCORT_MOTION_TYPE &&
            mm->GetDestination(dx, dy, dz))
        {
            if (std::sqrt((dx - x) * (dx - x) + (dy - y) * (dy - y) + (dz - z) * (dz - z)) <=
                BM_REPATH_EPSILON)
                return;  // gliding here already
            DcMovement::ResolveEscortConflict(bot);
        }

        // Same-destination re-issue floor (see BM_REISSUE_MS): the check above
        // cannot see a glide the combat engine has since layered MoveChase over.
        {
            struct LastIssue { float x, y, z; uint32 ms; };
            thread_local std::unordered_map<uint32, LastIssue> lastIssue;
            uint32 const guid = bot->GetGUID().GetCounter();
            auto it = lastIssue.find(guid);
            if (it != lastIssue.end())
            {
                LastIssue const& li = it->second;
                float const ddx = li.x - x, ddy = li.y - y, ddz = li.z - z;
                bool const sameDest =
                    std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz) <= BM_REPATH_EPSILON;
                if (sameDest && GetMSTimeDiffToNow(li.ms) < BM_REISSUE_MS)
                    return;
            }
            lastIssue[guid] = { x, y, z, getMSTime() };
        }

        if (dist > BM_LONG_HAUL)
        {
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            {
                ChunkedPathfinder::Result const path = LongRangePathfinder::Build(bot, x, y, z);
                // Outcome of the long haul, per bot, throttled. This is the ONLY
                // way to tell "the glide was issued and is running" from "the
                // route could not be built" from "the spline was issued and
                // something cancelled it".
                BmTravelLog(bot,
                            path.reachable
                                ? (path.segments.empty() ? "route reachable but EMPTY"
                                                         : "route ok -> issuing spline")
                                : "route UNREACHABLE",
                            dist, path.failureReason);
                if (path.reachable && !path.segments.empty())
                {
                    // Element 0 is the live position — the escort path[0]=start
                    // convention SplinePath expects.
                    Movement::PointsArray points;
                    points.push_back(G3D::Vector3(bot->GetPositionX(), bot->GetPositionY(),
                                                  bot->GetPositionZ()));
                    for (PathSegment const& seg : path.segments)
                    {
                        // A jump leg cannot be expressed as a ground spline. BM is
                        // flat swamp so this should never fire; deliver what we have.
                        if (seg.jumpDown || seg.jumpGap)
                            break;
                        for (G3D::Vector3 const& p : seg.polyline)
                        {
                            if (points.size() >= DungeonPathFollower::MAX_SPLINE_WINDOW_POINTS)
                                break;
                            points.push_back(p);
                        }
                    }
                    bool const issued = DcMovement::SplinePath(botAI, points);
                    BmTravelLog(bot, issued ? "spline ISSUED" : "spline REFUSED (paused/<2pts)",
                                dist, std::to_string(points.size()) + " pts");
                    if (issued)
                        return;
                }
            }
            // No navmesh route: fall through. MovePoint will not deliver at this
            // range either, but it is what the old code did, and the force-pull
            // below remains the backstop once mobs are actually next to us.
        }

        bot->GetMotionMaster()->MovePoint(0, x, y, z,
                                          FORCED_MOVEMENT_NONE, 0.0f, 0.0f,
                                          /*generatePath*/ true, false);
    }

    void BmGarrison(Player* bot, float x, float y, float z)
    {
        BmTravelTo(bot, x, y, z, BM_GARRISON_LEASH);
    }

    // Force a Black Morass mob into combat with the bot.
    //
    // Two things must both be true for the normal pipeline to pull one of the rift
    // adds, and neither is:
    //
    //  1. It will not aggro US. DoSummonAtRift spawns it REACT_DEFENSIVE and sends
    //     it to a home 14yd from Medivh, where it RESETS and casts Corrupt on
    //     itself. Defensive + at home + never damaged = it stands there
    //     indefinitely with the party in melee range.
    //  2. We do not reliably pull IT. EngageDirect ends with bot->Attack(), whose
    //     own comment concedes it "doesn't reciprocate combat until a swing lands
    //     — which stock combat targeting can drop the target before". It HAS a
    //     deterministic force-pull for exactly this, creature->EngageWithTarget(),
    //     but that call is gated `if (!bot->IsHostileTo(creature))` on the
    //     reasoning that a red-name hostile aggros on its own. These are red-name
    //     hostiles that do NOT aggro on their own — the one case that misses.
    //
    // Deliberately NOT fixed by loosening the shared gate in EngageDirect, nor by
    // loosening AttackersValue::IsPossibleTarget (which is why DcTargeting's
    // selector reports the volume empty while the party stands on the mob): both
    // are shared by every dungeon's engage path and their strictness is
    // load-bearing elsewhere. Scoped here to map 269 and to these entries instead.
    //
    // EngageWithTarget adds forced threat (or SetInCombatWith for a threat-less
    // unit), which pulls the mob out of its reset idle and hands it to the normal
    // combat engine; AttackStart makes its SmartAI actually swing rather than
    // merely hold threat. From that point it is an ordinary hostile and every
    // downstream gate sees it.
    // Point the TANK at `target` and start swinging.
    //
    // BmForcePull below is the other half of a pull and is not a substitute for
    // this one: EngageWithTarget/AttackStart make the CREATURE attack the BOT, and
    // nothing in that direction ever gives the bot a victim. A player does not
    // auto-retaliate — something has to call Player::Attack. Normally that is
    // stock combat's own targeting, but the driver has to be sure the tank commits
    // to the KEEPER specifically (it is the objective; everything else at the
    // portal is noise), and to commit on the tick it arrives rather than whenever
    // stock targeting next re-picks.
    //
    // Mirrors the in-range tail of DungeonClearEngageActionBase::EngageDirect
    // minus the parts only an Action can do (engine change, movement). Idempotent
    // by the GetVictim() guard, so calling it every tick never fights stock
    // combat's target management once the tank is already on the right mob.
    bool BmEngageTarget(Player* bot, AiObjectContext* context, Creature* target)
    {
        if (!bot || !target || !target->IsAlive())
            return false;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return false;

        bool const alreadyOnIt = bot->GetVictim() == target;
        if (!alreadyOnIt)
        {
            bot->SetSelection(target->GetGUID());
            if (!bot->HasInArc(CAST_ANGLE_IN_FRONT, target))
                ServerFacade::instance().SetFacingTo(bot, target);
            if (context)
                context->GetValue<Unit*>(DcKey::Stock::CurrentTarget)->Set(target);
            bot->Attack(target, botAI->IsMelee(bot));
        }

        // FLIP THE BOT ONTO THE COMBAT ENGINE. This is not a detail — without it
        // the tank swings and casts NOTHING, which is the whole "it attacks but
        // never does a rotation" failure.
        //
        // Engine transitions in mod-playerbots are ACTION-DRIVEN, not derived from
        // bot->IsInCombat(): PlayerbotAI::UpdateAI only auto-switches to and from
        // BOT_STATE_DEAD, and ChangeEngine(BOT_STATE_COMBAT) is called from exactly
        // two places in stock — AttackAction and PullActions. Player::Attack alone
        // just gives the bot a victim; the CORE then drives auto-attack swings with
        // no AI involvement whatsoever. So a bot that is force-attacked without the
        // engine flip sits on the NON-combat engine, where no class rotation action
        // exists to run, and melees the mob to death one white hit at a time with
        // no threat abilities. (It also strands this driver: the non-combat
        // conditional-event rung stands down on IsInCombat, and the combat copy
        // lives in a strategy the bot is not currently running.)
        //
        // DungeonClearEngageActionBase::EngageDirect ends with exactly these two
        // lines for exactly this reason; anything that force-attacks OUTSIDE that
        // helper has to repeat them.
        //
        // Deliberately NOT inside the !alreadyOnIt branch: once a bot has been left
        // attacking on the wrong engine, GetVictim() already equals the target, so
        // gating the flip on the target CHANGE would latch the broken state
        // permanently. ChangeEngine no-ops when the engine already matches, and the
        // react delay is only spent on a real transition.
        if (botAI->GetState() != BOT_STATE_COMBAT)
        {
            botAI->ChangeEngine(BOT_STATE_COMBAT);
            botAI->SetNextCheckDelay(sPlayerbotAIConfig.reactDelay);
        }
        return !alreadyOnIt;
    }

    bool BmForcePull(Player* bot, Creature* c)
    {
        if (!c || !c->IsAlive() || c->IsInCombat())
            return false;
        // It parked itself in the reset/home idle; clear that before the pull so
        // the AI does not immediately settle back into it.
        if (c->IsInEvadeMode())
            c->ClearUnitState(UNIT_STATE_EVADE);
        c->EngageWithTarget(bot);
        if (c->AI())
            c->AI()->AttackStart(bot);
        return true;
    }

    // Count the rift adds parked on (or arriving at) Medivh's ring — the party's
    // one real loss condition, since each of them is channelling Corrupt.
    uint32 BmDrainersOnMedivh(Creature* medivh)
    {
        if (!medivh)
            return 0;
        std::list<Creature*> found;
        medivh->GetCreatureListWithEntryInGrid(found, BlackMorassDrainEntries(),
                                               BM_MEDIVH_RING);
        uint32 alive = 0;
        for (Creature* c : found)
            if (c && c->IsAlive())
                ++alive;
        return alive;
    }

    // The rift this bot is working, LOCKED until it despawns.
    //
    // Selection has to be stable. `FindNearestCreature(NPC_TIME_RIFT)` re-decides
    // every tick, and with two to four rifts open at once "nearest" flips as the
    // tank moves — so the tank oscillates between portals and closes neither. That
    // is the shape the user reported: "they fall apart any time two portals or
    // more are open, they just can't make it to the portal".
    //
    // So: keep the locked rift while it still exists (its keeper dying despawns
    // it, which is exactly the right moment to re-pick), and otherwise choose the
    // nearest rift that ALREADY HAS a live keeper — the one that can be shut off
    // now — falling back to the nearest rift at all when none has spawned its
    // keeper yet. thread_local for the same reason as BmTravelLog: one map-update
    // thread owns its table, so it is race-free without a lock.
    Creature* BmSelectTargetRift(Player* bot)
    {
        thread_local std::unordered_map<uint32, ObjectGuid> locked;
        uint32 const guid = bot->GetGUID().GetCounter();

        auto it = locked.find(guid);
        if (it != locked.end())
        {
            if (Creature* held = ObjectAccessor::GetCreature(*bot, it->second))
                if (held->IsAlive())
                    return held;
            locked.erase(it);
        }

        std::list<Creature*> rifts;
        bot->GetCreatureListWithEntryInGrid(rifts, BM_NPC_TIME_RIFT, BM_RIFT_SCAN);

        Creature* bestWithKeeper = nullptr;
        Creature* bestAny = nullptr;
        float bestWithKeeperDist = 0.0f;
        float bestAnyDist = 0.0f;

        for (Creature* rift : rifts)
        {
            if (!rift || !rift->IsAlive())
                continue;
            float const d = bot->GetExactDist2d(rift);
            if (!bestAny || d < bestAnyDist)
            {
                bestAny = rift;
                bestAnyDist = d;
            }

            std::list<Creature*> keepers;
            rift->GetCreatureListWithEntryInGrid(keepers, BlackMorassKeeperEntries(),
                                                 BM_KEEPER_BAND);
            bool hasKeeper = false;
            for (Creature* k : keepers)
                if (k && k->IsAlive())
                {
                    hasKeeper = true;
                    break;
                }
            if (hasKeeper && (!bestWithKeeper || d < bestWithKeeperDist))
            {
                bestWithKeeper = rift;
                bestWithKeeperDist = d;
            }
        }

        Creature* pick = bestWithKeeper ? bestWithKeeper : bestAny;
        if (pick)
            locked[guid] = pick->GetGUID();
        return pick;
    }

    // The live keeper belonging to `rift`, or nullptr while it has not spawned yet
    // (the first 6s) or is already dead (the rift is about to despawn).
    Creature* BmKeeperOf(Creature* rift)
    {
        if (!rift)
            return nullptr;
        std::list<Creature*> keepers;
        rift->GetCreatureListWithEntryInGrid(keepers, BlackMorassKeeperEntries(),
                                             BM_KEEPER_BAND);
        Creature* best = nullptr;
        float bestDist = 0.0f;
        for (Creature* k : keepers)
        {
            if (!k || !k->IsAlive())
                continue;
            float const d = rift->GetExactDist2d(k);
            if (!best || d < bestDist)
            {
                best = k;
                bestDist = d;
            }
        }
        return best;
    }

    // --- The Black Morass: DriveBlackMorassEvent (hook id 8) --------------
    // The Custom step of the persistent "Defend Medivh" objective. It owns the
    // START and the END of the encounter; the WAVES are hook 12's job (the
    // conditional wave event preempts this one while anything is up).
    //
    //  * Aeonus DONE => Done. The objective clears and the run completes.
    //  * shield 0 => the run is LOST; end it here rather than sitting out the
    //    consolation lap (see the long note at the branch).
    //  * rift counter 0 (GetData(DATA_RIFT_NUMBER)) => the event has not started —
    //    either a fresh run or the instance's own 300s post-failure
    //    CleanupInstance() reset it. Nudge the tank INTO Medivh's 20yd trigger and
    //    let his own script start it. NEVER forge SetData(DATA_MEDIVH) here: the
    //    instance takes it unguarded and reschedules the portal task, so a second
    //    call while a rift is up would double-spawn. Proximity is the only safe
    //    (and blizzlike) path, and it self-heals the post-failure restart too.
    //  * otherwise => hold the Medivh-side hold point, which is where the adds
    //    come to us.
    ObjectiveArriveResult DriveBlackMorassEvent(Player* bot, AiObjectContext* /*context*/,
                                                DungeonBossInfo const& /*info*/)
    {
        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Running;  // not in the instance yet

        // Aeonus dead — event won (this run or an earlier one). Done.
        if (inst->GetBossState(BM_DATA_AEONUS) == DONE)
            return ObjectiveArriveResult::Done;

        // MEDIVH IS DEAD — the run is lost.
        //
        // The shield is the encounter's loss condition and GetData exposes it
        // directly. A 0 reading is unambiguous: the instance constructor and
        // CleanupInstance both set it to 100, and SetData(DATA_DAMAGE_SHIELD)
        // early-returns once it is already 0 — so the only way to read 0 is that
        // the drainers actually took it to 0, which sets EVENT_PREPARE, kills
        // Medivh (KillSelf) and despawns the field.
        //
        // Without this the run does not stop, it just stops being winnable: the
        // core waits 300s, runs CleanupInstance, respawns Medivh and replays the
        // whole encounter from rift 0. Measured 2026-07-24 that costs ~1400s per
        // attempt against a 3600s run budget, so a party that fails twice can no
        // longer finish inside the hour and simply burns the clock — every
        // timed-out run in that batch was this, reported as the useless verdict
        // "overall_timeout" instead of naming the real failure.
        //
        // Deliberately BEFORE the rift-counter branch: CleanupInstance resets the
        // counter to 0, so that branch would otherwise read the post-failure state
        // as "not started yet" and cheerfully walk the tank back in to replay it.
        if (inst->GetData(BM_DATA_SHIELD_PERCENT) == 0)
        {
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                DcActionShared::DisableDungeonClear(
                    botAI, "Medivh's shield hit 0 — the Dark Portal event failed");
            return ObjectiveArriveResult::Done;
        }

        if (inst->GetData(BM_DATA_RIFT_NUMBER) == 0)
        {
            // Not started (or reset by the post-failure cleanup). Walk into
            // Medivh's start trigger — but only while he is alive: during the
            // failure sequence his corpse lingers until CleanupInstance() respawns
            // him ~300s later, and until then there is nothing to trigger, so hold
            // at the hold point and wait it out.
            Creature* medivh = bot->FindNearestCreature(BM_NPC_MEDIVH, BM_MEDIVH_SCAN, /*alive*/ true);
            if (medivh)
            {
                // Distance to MEDIVH is the thing his script actually gates on, so
                // it is the thing this pushes on — and the thing it logs, so a
                // "walked in but never started" run names its own gap instead of
                // repeating an unfalsifiable "walking into the trigger".
                float const dist = bot->GetExactDist(medivh);
                if (dist > BM_START_HOLD)
                {
                    static uint32 lastStartLog = 0;
                    uint32 const now = getMSTime();
                    if (!lastStartLog || GetMSTimeDiffToNow(lastStartLog) > 10000)
                    {
                        lastStartLog = now;
                        LOG_DEBUG("playerbots.dungeonclear",
                                 "DungeonClear: Black Morass — walking {} into Medivh's 20yd "
                                 "proximity trigger to start the portal event (rift counter 0, "
                                 "dist {:.1f}yd, target {:.0f}yd)",
                                 bot->GetName(), dist, BM_START_APPROACH);
                    }

                    // Re-aimed every push at a near-point on the bot's side of
                    // Medivh: each re-issue plans a fresh path from wherever the
                    // last one actually ended, so a truncated approach converges
                    // instead of latching short. The leash is the arrival tolerance
                    // on the NEAR-POINT, so it has to be tight: the gate that
                    // actually matters is the 3D distance to Medivh re-tested above
                    // on the next tick.
                    float x, y, z;
                    medivh->GetNearPoint(bot, x, y, z, bot->GetObjectSize(),
                                         BM_START_APPROACH, medivh->GetAngle(bot));
                    BmTravelTo(bot, x, y, z, /*leash*/ 3.0f);
                }
                // Inside BM_START_HOLD: standing well within the 20yd radius.
                // MoveInLineOfSight only fires off a RELOCATION notify, so a tank
                // that arrived and stopped needs no further nudging — the arrival
                // itself is the notify. Just hold and let it fire.
            }
            else
                BmGarrison(bot, BM_HOLD_X, BM_HOLD_Y, BM_HOLD_Z);

            return ObjectiveArriveResult::Running;
        }

        // Event running: hold on Medivh between waves.
        BmGarrison(bot, BM_HOLD_X, BM_HOLD_Y, BM_HOLD_Z);
        return ObjectiveArriveResult::Running;
    }

    // --- The Black Morass: BmDriveWave (hook id 12) ------------------------
    // The whole wave brain, and the ONLY step of the conditional wave event. It
    // re-decides from live world state every tick rather than walking a step list,
    // because the priority here is not a sequence — it is a standing preference
    // that has to be re-evaluated as rifts open and adds arrive (see the block
    // comment at the top of this section for what the old step list could not
    // express).
    //
    // Priority, highest first:
    //
    //   0. ALWAYS (never gates, never moves anyone): force-pull any rift add
    //      within BM_PULL_RADIUS that is not already fighting. This is what makes
    //      "hold on Medivh" work at all — the adds are REACT_DEFENSIVE and would
    //      otherwise stand next to the party channelling.
    //   1. AEONUS is on the field (wave 18) => go kill him, and ignore his rift
    //      entirely. His death ends the encounter, so nothing else is worth a
    //      yard of travel.
    //   2. Medivh's ring is dirty and we have not yet closed on a keeper => go
    //      clean it. Adds parked there are the only thing that loses the run, and
    //      the ring is where every rift's output converges, so this is both the
    //      repair and the best place to be standing.
    //   3. A rift is open => go to its KEEPER and pull it. Killing the keeper
    //      despawns the rift and stops its 15s pump — the only shutoff there is.
    //      Before the keeper spawns (the first 6s) camp the rift itself, so it
    //      spawns on top of us and its adds do too.
    //   4. Nothing to do => hold the Medivh-side hold point and wait for the next
    //      rift.
    //
    // Rule 2 is bounded by BM_KEEPER_COMMIT so it cannot ping-pong the party: once
    // the tank has closed on the keeper it finishes it, and only then comes back.
    //
    // ARRIVED: kill the delivery glide.
    //
    // This driver sets StepsOwnMovement, which strips the driving action's per-tick
    // ResolveEscortConflict — that hold was cancelling the driver's own long-range
    // spline the tick after it was issued. Taking that responsibility means taking
    // the other half of it too: NOTHING else stops the glide, so deciding
    // "delivered" and simply yielding leaves a ~100yd escort spline in flight, and
    // it carries the tank on to wherever it was originally aimed.
    //
    // Live symptom: the tank ran past the rift keeper, all the way onto the portal,
    // then turned around and walked back to engage. Two things stacked. The spline
    // is launched while the rift is still QUIET, so it is aimed at the RIFT; when
    // the keeper spawns ~10yd away 6s later, BmTravelTo compares the in-flight
    // destination against the new one, finds them inside BM_REPATH_EPSILON (15yd),
    // and correctly declines to re-path — so the tank is still committed to the
    // rift. Arriving inside the keeper's delivery band then yielded WITHOUT
    // stopping, so the ride simply continued to the end of the line.
    //
    // ResolveEscortConflict only acts while an ESCORT glide is the active
    // generator, so this is a no-op once stopped and never perturbs the MoveChase
    // that closes the last few yards.
    void BmArrive(Player* bot)
    {
        DcMovement::ResolveEscortConflict(bot);
    }

    // Delivered at a portal: keep the tick or hand it over?
    //
    // IN COMBAT, hand it over. That is the whole reason the yield exists — the
    // stock combat engine needs the tick to pick a target, swing, cast and hold
    // threat, and it only gets one action per tick.
    //
    // OUT OF COMBAT, keep it. The rung below us is the "Defend Medivh" objective
    // (hook 8, DcRel::AtObjective), whose job between waves is to garrison the
    // Medivh-side hold point ~100yd away. Yield to it while standing on a portal
    // waiting for a keeper to spawn — the quiet 6s after a rift opens, or the beat
    // after one dies — and it cheerfully walks the tank straight back off the
    // portal. Holding the tick here is what pins the party on the rift through
    // that gap, which is the entire point of camping it.
    //
    // Nothing is lost by holding: out of combat there is no rotation to run, and
    // the between-waves rest happens in branch 3, where the driver and hook 8 both
    // want the tank at the same spot and the driver yields.
    ObjectiveArriveResult BmHoldPortal(Player* bot)
    {
        return bot->IsInCombat() ? ObjectiveArriveResult::Done
                                 : ObjectiveArriveResult::Running;
    }

    // RETURN CONTRACT — the tick is the scarce resource here.
    //
    // Engine::DoNextAction executes exactly ONE action per tick (it breaks out of
    // its loop on the first Execute that returns true), and this driver sits above
    // the stock combat movers in the COMBAT engine. So:
    //
    //   Running => "I am steering the tank right now." Claims the tick, which is
    //              what lets it take the tank off whatever it is fighting and walk
    //              it across the arena to the next rift.
    //   Done    => "Nothing to steer." YIELDS the tick (see the stepsOwnMovement
    //              branch in DcRunEventAction) so the stock combat engine can
    //              actually fight: pick a target, swing, cast, hold threat.
    //
    // Getting that backwards wiped parties. The first cut returned Running
    // unconditionally, so the tank drove to the portal, the keeper engaged it, and
    // the tank then stood there taking hits with no rotation and no threat while
    // the DPS pulled aggro and died. The force-pull below still runs on EVERY tick
    // either way — yielding costs it nothing.
    //
    // Never Blocked: a wipe or a corpse-run must never hard-stall the run, and the
    // event is Optional + Repeatable so a timeout simply re-fires it fresh.
    ObjectiveArriveResult BmDriveWave(Player* bot, AiObjectContext* context,
                                      DungeonBossInfo const& /*info*/)
    {
        if (!bot || !bot->GetMap())
            return ObjectiveArriveResult::Done;

        InstanceScript* inst = DcTargeting::GetInstanceScript(bot);
        if (!inst)
            return ObjectiveArriveResult::Done;

        // Won or lost — hook 8 owns both endings; stand down and let it run.
        //
        // Hook 8 sits at DcRel::AtObjective (30), below this event's rung (31), so
        // it cannot run while the wave event is still due. It does not have to
        // wait long: the core's failure sequence despawns the rifts at ~t+4.5s and
        // every summoned creature at ~t+6.5s (SetData(DATA_DAMAGE_SHIELD) steps
        // 2-3), at which point BmWaveHostilesActive reads false, this event stops
        // being due, and hook 8 fails the run. Deterministic, ~8s — not worth a
        // second copy of the disable funnel here.
        if (inst->GetBossState(BM_DATA_AEONUS) == DONE ||
            inst->GetData(BM_DATA_SHIELD_PERCENT) == 0)
            return ObjectiveArriveResult::Done;

        // --- 0. the standing force-pull -----------------------------------
        uint32 pulled = 0;
        {
            std::list<Creature*> adds;
            bot->GetCreatureListWithEntryInGrid(adds, BlackMorassDrainEntries(),
                                                BM_PULL_RADIUS);
            for (Creature* c : adds)
                if (BmForcePull(bot, c))
                    ++pulled;
        }

        Creature* medivh = bot->FindNearestCreature(BM_NPC_MEDIVH, BM_MEDIVH_SCAN, /*alive*/ true);
        uint32 const draining = BmDrainersOnMedivh(medivh);

        // Wave 18's boss. Looked up before the rift machinery because he preempts
        // all of it — see branch 1.
        Creature* aeonus = bot->FindNearestCreature(BM_NPC_AEONUS, BM_RIFT_SCAN, /*alive*/ true);

        Creature* rift = BmSelectTargetRift(bot);
        Creature* keeper = BmKeeperOf(rift);
        bool const atKeeper = keeper && bot->GetExactDist2d(keeper) <= BM_KEEPER_COMMIT;

        // One line per bot per 3s, carrying the four numbers that explain every
        // decision this hook makes. A run that goes wrong is diagnosed from this:
        // "draining>0 forever" means the sweep is not landing; "rift set, keeper
        // null forever" means we are camping a portal whose keeper we cannot see;
        // a shield falling with draining=0 means the ring radius is too tight.
        {
            thread_local std::unordered_map<uint32, uint32> lastMs;
            uint32& prev = lastMs[bot->GetGUID().GetCounter()];
            if (!prev || GetMSTimeDiffToNow(prev) > 3000)
            {
                prev = getMSTime();
                LOG_DEBUG("playerbots.dungeonclear",
                         "DungeonClear: Black Morass — {} wave: shield {}%, rift {}/18, "
                         "draining {}, pulled {}, keeper {}, atKeeper {}, aeonus {}",
                         bot->GetName(), inst->GetData(BM_DATA_SHIELD_PERCENT),
                         inst->GetData(BM_DATA_RIFT_NUMBER), draining, pulled,
                         keeper ? keeper->GetName() : "none", atKeeper,
                         aeonus ? "up" : "no");
            }
        }

        // --- 1. AEONUS IS UP: he IS the encounter --------------------------
        //
        // Wave 18 summons Aeonus and HIS DEATH WINS: boss_aeonus::JustDied ->
        // _JustDied -> SetBossState(DATA_AEONUS, DONE), which the instance turns
        // into Medivh's outro and this hook's own stand-down at the top. Once he
        // is on the field nothing else in the arena is worth a yard of travel, so
        // he outranks both branches below.
        //
        // He has to be named EXPLICITLY here because he fits neither of them:
        //
        //  * NOT A KEEPER. kBmKeeperEntries deliberately excludes him (he leaves
        //    the rift the instant he spawns), so BmKeeperOf(rift 18) is null
        //    forever and branch 3 falls through to "camp the rift and wait for a
        //    keeper", which never arrives. Camping is pointless there anyway:
        //    npc_time_rift::JustSummoned records _riftKeeperGUID from the first
        //    NON-Aeonus summon, so rift 18's shutoff is a trash add and killing
        //    the boss does not close it at all. THE WAVE-18 RIFT IS A DEAD END —
        //    ignore it.
        //  * IS A DRAINER. boss_aeonus::IsSummonedBy leaves him REACT_DEFENSIVE
        //    and walks him to a home 14yd from Medivh, so branch 2 does eventually
        //    pick him up once he has arrived and started draining — but only after
        //    the tank has been sent across the arena to the portal first. That
        //    two-hop dance is the live symptom the user reported: "on wave 18 the
        //    bots run to the portal, then turn around and engage the boss."
        //
        // So: find him anywhere in the arena and walk straight at him, intercepting
        // his march rather than waiting for him to reach Medivh (he drains at
        // DOUBLE the trash rate once he gets there). Same delivery band and the
        // same two-directional pull as a keeper — BmForcePull makes him attack US
        // (defensive + homing, he never aggros on his own), BmEngageTarget points
        // the tank at him and flips the combat engine.
        if (aeonus)
        {
            if (bot->GetExactDist2d(aeonus) > BM_DELIVERED_KEEPER)
            {
                BmTravelTo(bot, aeonus->GetPositionX(), aeonus->GetPositionY(),
                           aeonus->GetPositionZ(), BM_CAMP_LEASH);
                return ObjectiveArriveResult::Running;
            }

            // Delivered — stop the glide first (see BmArrive), then commit.
            BmArrive(bot);
            BmForcePull(bot, aeonus);
            BmEngageTarget(bot, context, aeonus);
            return BmHoldPortal(bot);
        }

        // --- 2. clean Medivh's ring ---------------------------------------
        if (draining > 0 && !atKeeper && medivh)
        {
            if (bot->GetExactDist2d(BM_HOLD_X, BM_HOLD_Y) > BM_DELIVERED_MEDIVH)
            {
                // Same band the guard just tested, so the two agree on one
                // number: travel to the hold point until DELIVERED. (The sibling
                // approach at the bottom of this hook reaches the same point via
                // BmGarrison, i.e. BM_GARRISON_LEASH=12 — either value behaves
                // identically here, since the guard above already stops calling
                // this once inside 20yd. Matching the guard is the clearer of
                // the two.)
                BmTravelTo(bot, BM_HOLD_X, BM_HOLD_Y, BM_HOLD_Z, BM_DELIVERED_MEDIVH);
                return ObjectiveArriveResult::Running;
            }
            // Inside the band: the drainers are already force-pulled above and are
            // running at us. Hand the tick to combat so the party kills them.
            BmArrive(bot);
            return ObjectiveArriveResult::Done;
        }

        // --- 3. close the rift --------------------------------------------
        if (rift)
        {
            if (keeper)
            {
                float const toKeeper = bot->GetExactDist2d(keeper);
                if (toKeeper > BM_DELIVERED_KEEPER)
                {
                    BmTravelTo(bot, keeper->GetPositionX(), keeper->GetPositionY(),
                               keeper->GetPositionZ(), BM_CAMP_LEASH);
                    return ObjectiveArriveResult::Running;
                }

                // Delivered — STOP FIRST. The glide in flight is very likely still
                // aimed at the RIFT (see BmArrive); riding it out is what made the tank
                // run past the keeper and then turn around.
                BmArrive(bot);

                // Delivered. Commit the tank to the KEEPER — it is the objective,
                // and everything else at the portal is noise stock targeting could
                // legitimately prefer. BmForcePull makes the keeper attack US;
                // BmEngageTarget is the other direction, and without it a player
                // simply never retaliates. Both are idempotent, so this is safe to
                // re-run on the ticks where the keeper has drifted out of melee.
                BmForcePull(bot, keeper);
                BmEngageTarget(bot, context, keeper);
                // Then get out of the way so MoveChase closes the last few yards
                // and the tank's rotation builds threat — but only once we are
                // actually FIGHTING. See BmHoldPortal.
                return BmHoldPortal(bot);
            }

            // Keeper not up yet (the quiet first 6s) or already dead (the rift is
            // despawning this tick). Camp the rift so both it and its adds spawn
            // on top of us.
            // Camp TIGHT here, unlike the keeper band: the point is to be standing
            // ON the spawn point when the keeper and its adds pop, and there is no
            // fight yet — so the loose band that stops a tank breaking melee to walk
            // two yards buys nothing here.
            if (bot->GetExactDist2d(rift) > BM_CAMP_LEASH)
            {
                BmTravelTo(bot, rift->GetPositionX(), rift->GetPositionY(),
                           rift->GetPositionZ(), BM_CAMP_LEASH);
                return ObjectiveArriveResult::Running;
            }
            BmArrive(bot);
            return BmHoldPortal(bot);
        }

        // --- 4. hold where the adds come to us -----------------------------
        if (bot->GetExactDist2d(BM_HOLD_X, BM_HOLD_Y) > BM_DELIVERED_MEDIVH)
        {
            BmGarrison(bot, BM_HOLD_X, BM_HOLD_Y, BM_HOLD_Z);
            return ObjectiveArriveResult::Running;
        }
        BmArrive(bot);
        return ObjectiveArriveResult::Done;
    }

    // --- The Shattered Halls: StartNethekurseIntro (hook id 9) ------------
    // Grand Warlock Nethekurse (16807) spawns SetImmuneToAll; the ONLY things
    // that clear it are ACTION_START_INTRO from area trigger 4347
    // (at_rp_nethekurse — CMSG_AREATRIGGER, which a bot never sends; same class
    // as the Ring of Law / Zum'rah bugs) or his own door-watch seeing chamber
    // door 182539 lockpicked open. The DC route teleports past that door's
    // navmesh break without touching it, so a full-bot run walks into a
    // permanently immune, passive first boss and the engage loop deadlocks.
    //
    // Driven by the Conditional "Wake Grand Warlock Nethekurse" event
    // (ShatteredHallsEvents, due only while he is alive, near, and still
    // IsImmuneToPC). Fire exactly what the area-trigger script fires:
    // DoAction(ACTION_START_INTRO) on his AI — it drops SetImmuneToAll on him
    // and his four peons and starts the peon-kill RP, and it carries its own
    // `_introStarted` guard so a duplicate fire (a lagging human's real
    // trigger, say) is a no-op. From there the normal flow takes over: the
    // tank engages him (JustEngagedWith cancels the RP), or he finishes the
    // peons and AttackStarts the nearest player himself.
    //
    // Logged at INFO for the same reason WakeZumrah is: this is the one place
    // a Shattered Halls first-boss deadlock gets fixed silently, and under the
    // test harness a bot-side failure is otherwise invisible.
    constexpr uint32 SHH_NETHEKURSE = 16807;
    constexpr int32 SHH_ACTION_START_INTRO = 0;  // boss_nethekurse.cpp ACTION_START_INTRO
    // Mirrors the event predicate's scan: the party is at/near him whenever the
    // event is due, so this always resolves the same creature the gate read.
    constexpr float SHH_NETHEKURSE_SCAN = 60.0f;

    ObjectiveArriveResult StartNethekurseIntro(Player* bot, AiObjectContext* /*context*/,
                                               DungeonBossInfo const& /*info*/)
    {
        Creature* nethekurse = bot->FindNearestCreature(SHH_NETHEKURSE, SHH_NETHEKURSE_SCAN);
        if (!nethekurse || !nethekurse->IsAlive())
            return ObjectiveArriveResult::Done;  // gone/dead — nothing to wake

        // Already woken (this fired, or a human's client tripped AT 4347).
        if (!nethekurse->IsImmuneToPC())
            return ObjectiveArriveResult::Done;

        nethekurse->AI()->DoAction(SHH_ACTION_START_INTRO);
        LOG_INFO("playerbots.dungeonclear",
                 "DungeonClear: Shattered Halls — started Nethekurse's ({}) intro for {} "
                 "(area trigger 4347 never fires for an all-bot party, so he stays immune)",
                 SHH_NETHEKURSE, bot->GetName());
        // Done even if the flip somehow failed to take: the Repeatable event's
        // predicate still reads immune next tick and re-fires this hook.
        return ObjectiveArriveResult::Done;
    }

    // --- Old Hillsbrad: GrantIncendiaryBombs (hook id 3) ------------------
    // Brazen (18725) only offers his drake ride to Durnholde Keep when the player
    // HOLDS the Pack of Incendiary Bombs (item 25853) — gossip menu 7959 option 0
    // is condition-gated on that item. A player gets the pack by talking to Erozion
    // (18723), but Erozion's grant option is quest-gated (quest 10283 "Taretha's
    // Diversion" taken/rewarded), which a bot never has — so the bot can never
    // select Erozion's gossip. Grant the pack directly (mechanically identical to
    // Erozion's AddItem) so Brazen's ride option then appears. Idempotent: Done
    // once the tank holds it. (The same pack is used to bomb the barrels in
    // objective 2 — the UseItemOnGO step also self-grants as a backstop.)
    constexpr uint32 OH_ITEM_INCENDIARY_BOMBS = 25853;

    ObjectiveArriveResult GrantIncendiaryBombs(Player* bot, AiObjectContext* /*context*/,
                                               DungeonBossInfo const& /*info*/)
    {
        if (bot->GetItemByEntry(OH_ITEM_INCENDIARY_BOMBS))
            return ObjectiveArriveResult::Done;
        bot->AddItem(OH_ITEM_INCENDIARY_BOMBS, 1);
        return bot->GetItemByEntry(OH_ITEM_INCENDIARY_BOMBS)
                   ? ObjectiveArriveResult::Done
                   : ObjectiveArriveResult::Running;  // bags full this tick — retry
    }

    // --- The Mechanar: GrantCacheKeyAndLoot (hook id 4) -------------------
    // The Cache of the Legion (GO 184465 normal / 184849 heroic) is a locked chest
    // (Lock.dbc 1706, a LOCK_KEY_ITEM lock) requiring the Cache of the Legion Key
    // (item 30438). Blizzard's key is formed by combining the two Jagged Crystals
    // the Gatewatchers drop (Gyro-Kill -> Blue 30436, Iron-Hand -> Red 30437) via
    // the crystals' on-use spell 36565 — but a bot never right-clicks a crystal,
    // and in a party the two crystals can land in different bags. Per the design
    // decision (grant the key), hand the leader key 30438 directly, then OPEN the
    // chest exactly the way the Deadmines cannon / Old Hillsbrad barrels do it:
    // USE the key item on the GO (CastItemUseSpell). This is load-bearing — the
    // original "grant key + let the stock loot pipeline open it" plan DEADLOCKED
    // (the tank parked at the cache and never looted). Two independent stock
    // failings sink that plan: (a) MaybeSkipUnworthyLoot blacklists every chest
    // with DungeonClear.IgnoreChests on (the default), and (b) even reached,
    // OpenLootAction's CanOpenLock has the LOCK_KEY_ITEM case COMMENTED OUT, so it
    // can never produce an opening spell for a key-item lock — and the core only
    // opens such a lock when the KEY ITEM ITSELF is the cast item (Spell::
    // CanOpenLock: m_CastItem->GetEntry() == lockInfo->Index[j]). So the leader
    // must use the key on the chest itself; the resulting Player::SendLoot fires
    // SMSG_LOOT_RESPONSE -> the always-on "store loot" handler auto-stores it.
    // The chest is consumable (leaves GO_READY once used), so the loot-state check
    // is the completion latch; idempotent + self-healing across an event restart.
    constexpr uint32 MECH_GO_CACHE_NORMAL = 184465;
    constexpr uint32 MECH_GO_CACHE_HEROIC = 184849;
    constexpr uint32 MECH_ITEM_CACHE_KEY  = 30438;  // on-use spell 3366 (OPEN_LOCK)
    constexpr float  MECH_CACHE_SEARCH    = 25.0f;
    // Cast reach: the OPEN_LOCK ends in Player::SendLoot, which range-checks the
    // GO's interact box (the Deadmines/Durnholde item-use plants measured failing
    // past ~6yd). Stay well inside it; the objective anchor sits ON the chest, but
    // arrival jitter can leave the tank a couple of yards out, so walk the final
    // gap in rather than spam-casting from range.
    constexpr float  MECH_CACHE_CAST_REACH = 4.0f;

    // True if a real human is in the leader's party (or is the lone actor). A member
    // counts as human when it has no PlayerbotAI, or a PlayerbotAI that reports
    // IsRealPlayer (a self-bot) — both are someone who will walk up and loot the
    // cache. A pure bot (PlayerbotAI, not a real player) does not.
    bool PartyHasRealPlayer(Player* bot)
    {
        auto const isHuman = [](Player* p) -> bool
        {
            if (!p)
                return false;
            PlayerbotAI* ai = GET_PLAYERBOT_AI(p);
            return !ai || ai->IsRealPlayer();
        };

        Group* group = bot->GetGroup();
        if (!group)
            return isHuman(bot);

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (isHuman(ref->GetSource()))
                return true;
        return false;
    }

    ObjectiveArriveResult GrantCacheKeyAndLoot(Player* bot, AiObjectContext* /*context*/,
                                               DungeonBossInfo const& /*info*/)
    {
        // Full-bot run: skip the open entirely. The cache (184465/184849) is a
        // CONSUMABLE chest that only despawns once its loot is actually LOOTED, and in
        // a bot party nobody takes it — the stock loot pipeline ignores chests
        // (DungeonClear.IgnoreChests). So opening it just leaves the chest cycling its
        // loot-state (a visible flicker) until the step times out ~2 min later, and
        // the loot is worthless to a bot party anyway. Complete the objective and let
        // the tank move straight on to the elevator. Only when a real player is in the
        // group — who will loot it — do we run the Blizzlike open below, so normal
        // runs are unchanged. (This is why the hang never appeared in human-led runs.)
        if (!PartyHasRealPlayer(bot))
            return ObjectiveArriveResult::Done;

        GameObject* cache = bot->FindNearestGameObject(MECH_GO_CACHE_NORMAL, MECH_CACHE_SEARCH);
        if (!cache)
            cache = bot->FindNearestGameObject(MECH_GO_CACHE_HEROIC, MECH_CACHE_SEARCH);

        if (!cache)
        {
            // No cache in range: either the tank has not arrived / scanned it yet,
            // or it has already been looted and despawned (consumable). Once we
            // have handed over the key the latter is true -> Done; otherwise keep
            // waiting to arrive.
            return bot->HasItemCount(MECH_ITEM_CACHE_KEY, 1)
                       ? ObjectiveArriveResult::Done
                       : ObjectiveArriveResult::Running;
        }

        // The chest leaves GO_READY the instant the key-use OPEN_LOCK lands (the
        // loot window is up and the "store loot" handler is draining it) — a
        // stable, idempotent "this cache is done" latch.
        if (cache->getLootState() != GO_READY)
            return ObjectiveArriveResult::Done;

        // The 2s key "Opening" cast is already running — let it complete rather
        // than re-issue and interrupt ourselves every tick.
        if (bot->IsNonMeleeSpellCast(false))
            return ObjectiveArriveResult::Running;

        // Grant the key (bots never combined the crystals).
        Item* key = bot->GetItemByEntry(MECH_ITEM_CACHE_KEY);
        if (!key)
        {
            bot->AddItem(MECH_ITEM_CACHE_KEY, 1);
            key = bot->GetItemByEntry(MECH_ITEM_CACHE_KEY);
            if (!key)
                return ObjectiveArriveResult::Running;  // bags full this tick — retry
        }

        // Close the final yards if the objective anchor left us just outside the
        // interact box (SendLoot's range check is unforgiving). Nothing else moves
        // the tank while the event holds, so this walk-in sticks.
        if (bot->GetExactDist(cache) > MECH_CACHE_CAST_REACH)
        {
            if (!bot->isMoving())
                bot->GetMotionMaster()->MovePoint(0, cache->GetPositionX(), cache->GetPositionY(),
                                                  cache->GetPositionZ());
            return ObjectiveArriveResult::Running;
        }

        // USE the key ON the chest: item-use supplies m_CastItem so the KEY_ITEM
        // lock opens -> SendLoot -> SMSG_LOOT_RESPONSE -> "store loot" stores it.
        // Feral-form druids can't cast the item spell at all — shift back first
        // (same gate the barrel plants use). See DcFormGate.
        DcFormGate::DropBlockingForm(bot, key);
        SpellCastTargets targets;
        targets.SetGOTarget(cache);
        bot->CastItemUseSpell(key, targets, 0, 0);
        return ObjectiveArriveResult::Running;  // the loot-state latch above confirms it
    }

    // hookId -> behaviour. To give an objective on-arrival behaviour, add a row
    // here and reference its id from a BossRosterRegistry objective (onArriveHook)
    // or a Custom event step (DungeonEventRegistry).
    std::unordered_map<uint32, ObjectiveHookRegistry::Hook> const& Hooks()
    {
        static std::unordered_map<uint32, ObjectiveHookRegistry::Hook> const kHooks = {
            { 1, &EnsureRingStarted },       // BRD Ring of Law — start the arena event
            { 2, &FireDefiasCannon },        // Deadmines — fire the cannon, open the door
            { 3, &GrantIncendiaryBombs },    // Old Hillsbrad — pack of bombs (unlocks Brazen)
            { 4, &GrantCacheKeyAndLoot },    // The Mechanar — Cache of the Legion key + loot
            { 5, &WakeZumrah },              // ZulFarrak — flip Zum'rah hostile (AT 962)
            { 6, &DriveMellicharWaves },     // Arcatraz — poke Mellichar, hold through the waves
            { 7, &DriveAnzuSummon },         // Sethekk Halls — force-summon Anzu (send-event 14797)
            { 8, &DriveBlackMorassEvent },   // Black Morass — start + garrison the portal event
            { 9, &StartNethekurseIntro },    // Shattered Halls — fire Nethekurse's client-only intro
            // 10 and 11 (BmCampActivePortal / BmPullDrainers) were the old Black
            // Morass wave pair and are RETIRED, folded into hook 12. Splitting
            // "camp the portal" from "pull the drainers" across two blocking steps
            // of one sequential list is what let a live rift's 15s add pump wedge
            // the list before it ever reached the keeper. The ids are left unused
            // rather than recycled so an old log line naming them stays legible.
            { 12, &BmDriveWave },            // Black Morass — the whole wave driver
        };
        return kHooks;
    }
}

ObjectiveArriveResult ObjectiveHookRegistry::Run(uint32 hookId, Player* bot, AiObjectContext* context,
                                                 DungeonBossInfo const& info)
{
    if (hookId == 0)
        return ObjectiveArriveResult::Done;

    auto const& hooks = Hooks();
    auto it = hooks.find(hookId);
    if (it == hooks.end() || !it->second)
    {
        // A non-zero hookId that resolves to nothing is a wiring bug (a typo, or a
        // hook that was removed). Latching Done would silently pass a step/objective
        // authored to do real work; Blocked surfaces it (the run stalls for the
        // human) and the throttled warn names the id. Registry-integrity gtest
        // catches this at author time; this is the runtime backstop.
        static std::atomic<uint32> lastWarnMs{0};
        uint32 const now = getMSTime();
        uint32 prev = lastWarnMs.load(std::memory_order_relaxed);
        if (now - prev > 10000u &&
            lastWarnMs.compare_exchange_strong(prev, now, std::memory_order_relaxed))
        {
            LOG_WARN("playerbots", "DungeonClear: objective hookId {} is not registered "
                                   "(broken wiring) -> Blocked", hookId);
        }
        return ObjectiveArriveResult::Blocked;
    }

    return it->second(bot, context, info);
}

bool ObjectiveHookRegistry::Has(uint32 hookId)
{
    if (hookId == 0)
        return false;
    auto const& hooks = Hooks();
    auto it = hooks.find(hookId);
    return it != hooks.end() && it->second;
}
