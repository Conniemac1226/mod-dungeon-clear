/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCSMARTRESTDECISION_H
#define _PLAYERBOT_DCSMARTRESTDECISION_H

#include <cstddef>
#include <cstdint>
#include <vector>

// Pure decision kernel for Smart Rest — the hysteresis rest latch. The legacy
// rest gate is a single threshold used for BOTH ends (the party stops whenever
// anyone is below RestHealthPct/RestManaPct and rests back up to that same
// value), which produces constant micro-rests. Smart Rest splits the two ends:
// a LOW, role-based trigger (DPS mana, tank mana, healer mana, any-role HP)
// latches a party-wide rest, and the release bar is full health but only a
// near-full mana. A zero mana trigger disables that role for entry and release,
// including at bosses. Human players may be excluded entirely, leaving only
// autonomous bots to start or hold a rest. Between rests eating/drinking is
// fully suppressed. A boss pull raises each enabled mana role's entry to its
// release bar.
//
// Extracted engine-free so it is unit-testable in isolation, mirroring
// DecidePull / DecideCombatRegroup. DcSmartRest (the glue) gathers the live
// group into Member snapshots, calls Decide against the latch stored in the
// leader's DcRunState, and writes the verdict back. Nothing here touches a
// Player/Unit/context, so no game headers are needed.

namespace DcSmartRestDecision
{
    // HP release bar, every member: "full", float-safe. GetHealthPct/
    // GetPowerPct return 100.0f exactly when full (cur/max with cur==max), but a
    // buff or aura shifting the max pool mid-rest could strand a bot at 99.x
    // forever — half a percent of slack costs nothing perceptible.
    constexpr float kReleasePct = 99.5f;

    // Mana release bar, every enabled mana role: deliberately short of full. The last
    // sliver of mana regenerates for free while the party walks to the next
    // pack, so holding the whole party at rest to claw back the final ~10% just
    // burns real time for no combat benefit. 90% stays far above every mana
    // trigger (tank 10 / healer 40 by default), so hysteresis holds — a release can never
    // instantly re-latch. HP keeps the full kReleasePct bar (a low HP bar would
    // send bots into the next pull hurt).
    constexpr float kManaReleasePct = 90.0f;

    // When includeHumans is on, humans hold to the SAME bars as bots. When it
    // is off, a human can neither start nor hold the latch on health or mana;
    // normal party-spread checks still keep the tank from leaving them behind.

    // One living, same-map group member, snapshotted by the glue. Dead and
    // off-map members are the snapshot builder's job to exclude, not ours.
    struct Member
    {
        float hpPct = 100.0f;
        float manaPct = 100.0f;   // meaningful only when isManaUser
        bool  isManaUser = false; // powerType == POWER_MANA && maxMana > 0
        bool  isHealer = false;   // PlayerbotAI::IsHeal — selects the mana trigger
        bool  isTank = false;     // elected DC leader — selects the tank trigger
        bool  isBot = false;      // PlayerbotAI exists and !IsRealPlayer()
    };

    struct Inputs
    {
        bool          latched = false;     // stored latch (DcRunState::smartRestLatched)
        std::uint32_t restElapsedMs = 0;   // now - smartRestSinceMs; 0 when not latched
        bool          rearmed = true;      // false during the post-timeout cooldown
        bool          includeHumans = true; // SmartRestIncludeHumans
        float         hpTriggerPct = 50.0f;       // SmartRestHealthPct (all roles)
        float         dpsManaTriggerPct = 0.0f;  // SmartRestDpsManaPct
        float         tankManaTriggerPct = 10.0f; // SmartRestTankManaPct
        float         healerManaTriggerPct = 40.0f;  // SmartRestHealerManaPct
        std::uint32_t maxRestMs = 0;       // timeout failsafe; 0 = never time out

        // The next pull is a BOSS and the tank is inside its engage range. Raises
        // each enabled mana role's entry to its release bar. A role configured at
        // zero remains ignored. Mana only; HP has no boss-specific bar.
        bool          bossPull = false;
    };

    struct Result
    {
        bool latched = false;
        bool timedOut = false;             // released by the failsafe this eval
        std::vector<std::size_t> blockers; // members below trigger (entering) or
                                           // below their release bar (holding)
    };

    // The member's mana trigger for its role. 0 = that dimension disabled.
    float ManaTriggerPct(Member const& m, Inputs const& in);

    // Release bars for one participating member: kReleasePct on HP and
    // kManaReleasePct on mana when that role's trigger is enabled. Mana is 0
    // for non-mana users and roles whose mana trigger is zero.
    float HpReleaseBar(Member const& m, Inputs const& in);
    float ManaReleaseBar(Member const& m, Inputs const& in);

    // The two halves of the hysteresis, exposed so the glue's DescribeWait can
    // name blockers with the exact same rules Decide applies. BelowTrigger also
    // owns the boss-pull entry (in.bossPull raises the mana entry to the mana
    // release bar).
    bool BelowTrigger(Member const& m, Inputs const& in);
    bool BelowRelease(Member const& m, Inputs const& in);

    // The verdict:
    //   Not latched: latch when rearmed and ANY member is BelowTrigger.
    //   Latched:     time out when maxRestMs > 0 and restElapsedMs >= maxRestMs;
    //                otherwise release only when NO member is BelowRelease.
    // Empty member list never latches.
    Result Decide(Inputs const& in, std::vector<Member> const& members);
}

#endif  // _PLAYERBOT_DCSMARTRESTDECISION_H
