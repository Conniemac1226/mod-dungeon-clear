/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCTESTRUNRECORD_H
#define _PLAYERBOT_DCTESTRUNRECORD_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "TestRun/DcDiagSnapshot.h"

// One JSON line per completed test run, appended to dc_testruns.jsonl in the
// worldserver working directory (env override DC_TESTRUNS_FILE) — same
// pattern as the decision-capture logs (DungeonClearApproachIo), but with
// real string escaping because reasons/details are free text. The dashboard
// tails this file for the run-history panel.

namespace DcTestRunRecord
{
    struct CompEntry
    {
        std::string name;
        std::string className;   // "warrior", ...
        std::string spec;        // premade-spec name actually applied
        std::string role;        // "tank" | "heal" | "dps"
        std::uint64_t guid = 0;
        std::uint32_t level = 0; // actual post-factory level (may be clamped)
    };

    struct StatusEntry
    {
        std::uint32_t t = 0;     // seconds since run start
        std::string state;       // publisher stateStr token
        std::string detail;
    };

    struct BossKill
    {
        std::uint32_t t = 0;
        std::uint32_t entry = 0; // 0 for anchor/objective completions
        std::string name;
        std::string via;         // "mask" | "anchor"
    };

    struct PauseEntry
    {
        std::uint32_t t = 0;
        std::string reason;
    };

    struct Record
    {
        // 3: added diag (full failure snapshot) + stallAtEnd/phaseAtEnd
        // 4: added heroic (run executed at DUNGEON_DIFFICULTY_HEROIC)
        // 5: added the wipe post-mortem (wipeOnBoss/wipeOpponent*)
        std::uint32_t schema = 5;
        std::string runId;
        std::string planId;       // owning `.dc test plan`, "" for ad-hoc runs
        std::string dungeon;      // registry token
        std::string dungeonName;
        std::string wing;
        std::uint32_t mapId = 0;
        std::uint32_t instanceId = 0;
        std::uint32_t level = 0;  // requested level
        bool heroic = false;      // run at DUNGEON_DIFFICULTY_HEROIC
        std::uint32_t compSeed = 0;  // seed BuildComp rolled this comp from (for replay)
        std::vector<CompEntry> comp;
        std::uint64_t startedAtMs = 0;  // unix ms
        std::uint64_t endedAtMs = 0;
        std::uint32_t durationS = 0;
        std::string result;       // DcTestRun::VerdictName token
        std::string failReason;   // human sentence; "" on success
        std::string disableReason;// verbatim DisableDungeonClear reason, if any
        std::uint32_t bossesTotal = 0;
        std::uint32_t bossesKilled = 0;
        std::vector<BossKill> bossTimeline;
        std::vector<StatusEntry> statusTimeline;
        std::vector<PauseEntry> pauses;
        std::uint32_t finalMap = 0;
        float finalX = 0.f, finalY = 0.f, finalZ = 0.f;
        std::uint32_t pauseGraceS = 0, stallGraceS = 0, noProgressS = 0, overallS = 0;
        std::string setupStage;   // non-empty only for setup_failed:* records

        // The stall reason and phase token as they stood when the run ended.
        // Previously only the stalled_timeout verdict preserved the stall
        // reason (it was folded into failReason); every other failure threw it
        // away, which is exactly the field a no_progress post-mortem wants.
        std::string stallAtEnd;
        std::string phaseAtEnd;

        // What the party was fighting when it went down. The first question
        // anyone asks of a failed run is "did we lose the boss fight, or did a
        // trash pack eat us?", and neither the verdict token nor the status
        // timeline answers it: combat ends the instant the last member dies, so
        // by teardown there is nothing left to read.
        //
        // Populated on the "wipe" verdict and on a death bailout ("disabled"
        // with a corpse still in the party); empty on every other outcome, and
        // empty on a wipe that happened out of combat (a hazard, a fall).
        bool wipeOnBoss = false;
        std::uint32_t wipeOpponentEntry = 0;
        std::string wipeOpponent;  // boss or trash-mob name

        // Full end-of-run diagnostic snapshot, captured before teardown
        // disbands the party. Serialized under "diag"; absent on records
        // written before schema 3 and on setup failures (no party existed).
        DcDiag::Snapshot diag;
    };

    // A thrashing run can flip status every publisher tick; keep the head
    // (how the run opened) and the tail (how it ended) and drop the middle.
    inline constexpr std::size_t kStatusHead = 50;
    inline constexpr std::size_t kStatusTail = 150;

    std::string EscapeJson(std::string_view s);
    std::string ToJsonl(Record const& rec);

    std::string CapturePath();
    void Append(Record const& rec);

    // Live-heartbeat sidecar for the dashboard: while a run is active the
    // manager overwrites this small JSON file every couple of seconds
    // (stage, elapsed, bosses, last status); the dashboard polls it for the
    // in-progress view. Env override DC_TESTRUN_LIVE_FILE.
    std::string LivePath();
}

#endif  // _PLAYERBOT_DCTESTRUNRECORD_H
