/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCEVENTDOORREGISTRY_H
#define _PLAYERBOT_DCEVENTDOORREGISTRY_H

#include "Common.h"

// Per-ENTRY list of door gameobjects that are SCRIPT-ONLY: the live client
// refuses a direct player open and ONLY an in-game event opens them, even though
// their template (an empty lock-85, the same template as plenty of plainly
// clickable doors) reads as openable to BotCanOpenDoorLikePlayer / DcDoorPolicy.
// A bot generic-Use()ing one of these toggles the server GO state while the
// client still treats the door as shut — a desync — and it also skips the
// intended event (e.g. Shadowfang Keep's courtyard door, which only opens when a
// freed prisoner walks over and unlocks it).
//
// This is DELIBERATELY keyed by GO ENTRY, not by lock id: lock 85 is shared with
// many doors bots SHOULD open (Deadmines Factory/Foundry/Mast Room, etc.), so a
// lock-level rule would break them. Keep this list to doors verified to be
// script/event-opened only; the door-blocked action consults it before deciding
// it is "entitled" to open a door, and leaves a listed door for the events
// framework or the human instead.
namespace DcEventDoorRegistry
{
    inline bool IsScriptOnly(uint32 goEntry)
    {
        switch (goEntry)
        {
            case 18895:  // Shadowfang Keep — Courtyard Door (freed-prisoner event)
            // Shadowfang Keep's other two gates, both empty-lock-85 like the
            // Courtyard Door and both driven purely by SmartAI:
            //
            //   18972 Sorcerer's Gate (guid 33785) — the Fenrus room's east exit
            //     toward Nandos. It opens on 'Arugal's Voidwalker (4627) - On Just
            //     Died - Set GO State'. The intended sequence is: Fenrus (4274)
            //     dies -> his SmartAI sets data on Archmage Arugal (4275) -> that
            //     runs timed actionlist 427500, which summons the four voidwalkers
            //     6s later -> killing one opens the gate. Force-opening the gate
            //     skipped that whole mechanic: the party walked out ~6s before the
            //     adds existed, then the voidwalkers spawned BEHIND it at the room's
            //     west end and the run wedged between advancing to Nandos and
            //     turning back for them (live report 2026-08-01). The gate has
            //     door.autoCloseTime 0, so a bot click opened it permanently.
            //     The "Arugal's Voidwalkers" event (map 33 id 3) now drives the
            //     real sequence.
            //
            //   18971 Arugal's Lair (guid 33241) — opens on 'Wolf Master Nandos
            //     (3927) - On Just Died - Set GO State'. Nandos stands 2.6yd in
            //     FRONT of it, so the ordinary run kills him and the door opens
            //     itself; a bot that force-opened it could instead walk straight
            //     past him to Archmage Arugal and skip an encounter. The
            //     door-blocked watchdog in DcEngageActions already named this door
            //     as the reason it exists — this is the entry that fixes it.
            case 18971:  // Shadowfang Keep — Arugal's Lair (opens on Nandos' death)
            case 18972:  // Shadowfang Keep — Sorcerer's Gate (voidwalker event)
                return true;
            default:
                return false;
        }
    }

    // Doors NAVIGATION must ignore entirely: never flagged as a corridor
    // blocker, never opened, never a reason to park or auto-pause. These are
    // interact-THROUGH gates — the run's objective is completed from the
    // players' side of the shut door (a gossip through the bars), after which
    // the event script opens the door itself. Flagging one as blocking is
    // always wrong: the route intentionally ends beside it, and the pause
    // machinery would halt a run that needs nothing from the door at all.
    inline bool IsNavigationIgnored(uint32 goEntry)
    {
        switch (goEntry)
        {
            case 184393:  // Old Hillsbrad — Thrall's Prison Door (gossip through
                          // the gate; his script opens it via EVENT_OPEN_DOORS)
                return true;
            // The Steamvault — Main Chambers Access Panels. These are wall
            // CONTROLS, not doors, but their template is GAMEOBJECT_TYPE_DOOR
            // and they spawn (and permanently stay) in GO_STATE_READY, so the
            // closed-door predicate reads each one as a shut gate sitting on
            // the corridor. Clicking one runs go_main_chambers_access_panel's
            // OnGossipHello, which returns true BEFORE GameObject::Use reaches
            // UseDoorOrButton — so the panel's own GOState never flips, and the
            // door-blocked action concluded "clicked it, still closed, can't
            // open" and auto-paused the run 13.8yd from its objective (live run
            // 2026-07-20, tank Fedrel). The panel is opened by nothing and
            // blocks nothing; the Steamvault event (map 545 id 1) clicks it,
            // which is what opens the real Main Chambers Door (183049).
            case 184125:  // Hydromancer Thespia's panel
            case 184126:  // Mekgineer Steamrigger's panel
                return true;
            default:
                return false;
        }
    }

    // Doors whose KEY requirement we deliberately waive: the bot opens them as
    // if it held the key, no item in inventory needed.
    //
    // Scarlet Monastery's Armory (Herod's Door) and Cathedral (Chapel Door)
    // both sit on lock 299 — Scarlet Key (7146) or lockpicking 175. A tank bot
    // carries neither, so an autonomous SM run parked at the wing entrance and
    // auto-paused every time, making those two wings unclearable without a
    // human handing the key over first. The doors are otherwise ordinary
    // traversal gates: no ScriptName, no AIName, no instance-script GO-state
    // control, and nothing behind them the key is meant to gate beyond the
    // wing itself (the key is a convenience item players farm from the
    // Graveyard/Library side, not an encounter lock).
    //
    // Keyed by GO ENTRY, not by lock id, for the same reason as the lists
    // above: lock 299 is shared with the Stratholme Scarlet-side doors, which
    // keep their key requirement.
    inline bool IsKeyExempt(uint32 goEntry)
    {
        switch (goEntry)
        {
            case 101854:  // Scarlet Monastery — Herod's Door (Armory, lock 299)
            case 104591:  // Scarlet Monastery — Chapel Door (Cathedral, lock 299)
                return true;
            default:
                return false;
        }
    }

    // The MIRROR-IMAGE special case: door gameobjects carrying NO lock at all
    // (template lockId 0) that a player nonetheless opens by simply clicking
    // them — ordinary traversal gates the dungeon expects you to walk through.
    //
    // BotCanOpenDoorLikePlayer otherwise refuses every lock-free door, because
    // lockId 0 is ALSO the shape of script/event seals the bot must not pop
    // (Uldaman's Seal of Khaz'Mul, lock-free and only opened by the keystone
    // event, isn't flagged GO_FLAG_NOT_SELECTABLE until its encounter is done,
    // so the generic flag screen can't tell them apart). We can't relax the
    // lock-free rule wholesale; instead we allowlist the entries verified in
    // the world DB to be plain clickable doors — no ScriptName, no AIName, no
    // instance-script GO-state control, no SmartAI.
    //
    // Scholomance's Iron Gates (175611-175618, 175620) and plain interior Doors
    // (175610, 175619) are exactly this: lock-free, scriptless room-to-room
    // gates the player clicks open. (The dungeon's *event* gates — Kirtonos
    // 175570 and the seven Gandling gates 177371-177377 — are deliberately
    // EXCLUDED; the instance script drives their state.)
    inline bool IsLockFreeClickable(uint32 goEntry)
    {
        switch (goEntry)
        {
            // Scholomance — interior traversal gates/doors (map 289)
            case 175610:  // Door
            case 175611:  // Iron Gate
            case 175612:  // Iron Gate
            case 175613:  // Iron Gate
            case 175614:  // Iron Gate
            case 175615:  // Iron Gate
            case 175616:  // Iron Gate
            case 175617:  // Iron Gate
            case 175618:  // Iron Gate
            case 175619:  // Door
            case 175620:  // Iron Gate
                return true;
            default:
                return false;
        }
    }
}

#endif
