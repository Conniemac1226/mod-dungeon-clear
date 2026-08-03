/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcRezRecovery.h"

#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Util/DcCombatFlag.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcStatusPublisher.h"

#include <string>
#include <vector>

#include "Group.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"

namespace
{
    using DcRezDecision::Member;

    bool IsRezClass(Player const* p)
    {
        switch (p->getClass())
        {
            case CLASS_PRIEST:
            case CLASS_PALADIN:
            case CLASS_SHAMAN:
            case CLASS_DRUID:
                return true;
            default:
                return false;
        }
    }

    // The member whose DcRunState owns this run — DEAD OR ALIVE. FindLeaderTank
    // only elects among alive tank bots, so it goes null (or resolves a tank
    // whose own run state is default) precisely in the case recovery matters
    // most: the leader is the corpse. Fall back to scanning the same-map group
    // for the bot whose own run state is enabled. See the header comment.
    Player* ResolveRunOwner(Player* bot)
    {
        auto owns = [](Player* p) -> bool
        {
            if (!p)
                return false;
            PlayerbotAI* ai = GET_PLAYERBOT_AI(p);
            return ai && DcRun::Of(ai).enabled;
        };

        if (Player* leader = DcLeaderSignal::FindLeaderTank(bot))
            if (owns(leader))
                return leader;

        Group* group = bot ? bot->GetGroup() : nullptr;
        if (!group)
            return owns(bot) ? bot : nullptr;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member->GetMapId() != bot->GetMapId())
                continue;
            if (owns(member))
                return member;
        }
        return nullptr;
    }

    // Same-map group snapshot, KEEPING dead members (unlike the Smart Rest
    // snapshot — the corpses are the whole point here). `players` receives the
    // matching Player* per row so verdict indices resolve to live identities.
    void BuildSnapshot(Player* bot, Player* owner, std::vector<Member>& out,
                       std::vector<Player*>& players)
    {
        auto add = [&](Player* member)
        {
            Member m;
            m.isDead = member->isDead();
            m.canRezClass = IsRezClass(member);
            m.isHealerRole = PlayerbotAI::IsHeal(member);
            m.isTankRole = PlayerbotAI::IsTank(member);
            m.isBot = GET_PLAYERBOT_AI(member) != nullptr;
            out.push_back(m);
            players.push_back(member);
        };

        Group* group = bot->GetGroup();
        if (!group)
        {
            add(owner ? owner : bot);
            return;
        }
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member->GetMapId() != bot->GetMapId())
                continue;
            add(member);
        }
    }

    // Instantaneous "is the party fighting" read for the clock freeze. The
    // latched variant (IsPartyEngagedLatched) is file-local to DcLeaderSignal;
    // a bare read is safe HERE because a false flicker merely clears and
    // re-stamps the clock — which RESETS the timeout budget, the forgiving
    // direction — it can never expire recovery early.
    //
    // ENGAGEMENT, not the combat flag: a resurrection cast is refused while
    // flagged ("rez party: cast 'resurrection' -> not possible yet"), so under a
    // phantom flag — a 45yd hostile area aura with nothing aggroed — the recovery
    // can never complete AND the flag froze this very clock, so the timeout that
    // is supposed to bound it never arrived. The run then held over the corpse
    // forever, inside the aura (Arcatraz heroic, tr-20260801-194932-20: hand-
    // rezzed by the player to unstick it). A real fight still freezes the clock.
    bool AnyMemberEngaged(std::vector<Player*> const& players)
    {
        for (Player* p : players)
            if (DcCombatFlag::IsEngaged(p))
                return true;
        return false;
    }

    DcRezRecovery::Plan EvaluateImpl(Player* bot, bool mutate)
    {
        DcRezRecovery::Plan plan;
        if (!bot)
            return plan;

        Player* owner = ResolveRunOwner(bot);
        PlayerbotAI* ownerAI = owner ? GET_PLAYERBOT_AI(owner) : nullptr;
        if (!ownerAI)
            return plan;
        DcRunState& run = DcRun::Of(ownerAI);
        if (!run.enabled || run.paused)
            return plan;  // a pause holds recovery too; clocks stay as-is

        std::vector<Member> members;
        std::vector<Player*> players;
        BuildSnapshot(bot, owner, members, players);

        uint32 const now = getMSTime();

        bool anyDead = false;
        for (Member const& m : members)
            anyDead = anyDead || m.isDead;

        if (!anyDead)
        {
            if (mutate && (run.rezPendingSinceMs || run.rezAnnounceMs))
            {
                // A recovery episode just completed — everyone is back up. The
                // rezzed member's low HP/mana now holds the tank via the
                // default between-pulls readiness floors until they recover.
                if (run.rezAnnounceMs)
                    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                        DcStatusPublisher::SendAddonMessage(
                            botAI, "CHAT\tParty restored \xe2\x80\x94 resuming.");
                run.rezPendingSinceMs = 0;
                run.rezAnnounceMs = 0;
            }
            return plan;  // None / NoDeaths
        }

        // First corpse by group order, for the disable messages.
        for (std::size_t i = 0; i < members.size(); ++i)
            if (members[i].isDead)
            {
                plan.deadName = players[i]->GetName();
                break;
            }

        if (!DcRezRecovery::Enabled(bot))
        {
            // Feature off: hand the party-died trigger its classic verdict.
            plan.verdict.outcome = DcRezDecision::Outcome::Disable;
            plan.verdict.reason = DcRezDecision::Reason::Disabled;
            return plan;
        }

        // The recovery clock: runs only while nobody is FIGHTING (a fight clears
        // it, so a mid-recovery add pull resets the budget instead of burning it).
        bool const partyEngaged = AnyMemberEngaged(players);
        if (mutate)
        {
            if (partyEngaged)
                run.rezPendingSinceMs = 0;
            else if (run.rezPendingSinceMs == 0)
                run.rezPendingSinceMs = now ? now : 1;
        }

        DcRezDecision::Inputs in;
        in.enabled = true;
        in.nowMs = now;
        in.pendingSinceMs = run.rezPendingSinceMs;
        in.timeoutMs = DcSettings::GetUInt(bot, "PostCombatRezTimeoutSecs") * 1000;
        in.partyEngaged = partyEngaged;

        plan.verdict = DcRezDecision::Decide(in, members);

        if (plan.verdict.rezzerIdx >= 0 &&
            plan.verdict.rezzerIdx < static_cast<int>(players.size()))
        {
            plan.rezzer = players[plan.verdict.rezzerIdx]->GetGUID();
            plan.rezzerName = players[plan.verdict.rezzerIdx]->GetName();
        }
        if (plan.verdict.targetIdx >= 0 &&
            plan.verdict.targetIdx < static_cast<int>(players.size()))
        {
            plan.target = players[plan.verdict.targetIdx]->GetGUID();
            plan.targetName = players[plan.verdict.targetIdx]->GetName();
        }

        // One announcement per recovery episode, from whichever member
        // evaluates first (the stamp on the shared run state dedupes the rest).
        if (mutate && plan.verdict.outcome == DcRezDecision::Outcome::Hold &&
            run.rezAnnounceMs == 0)
        {
            run.rezAnnounceMs = now ? now : 1;
            std::string line;
            if (plan.verdict.reason == DcRezDecision::Reason::WaitingOnHuman)
                line = plan.targetName + " died \xe2\x80\x94 waiting for you to resurrect them (" +
                       std::to_string(in.timeoutMs / 1000) + "s).";
            else
                line = plan.targetName + " died \xe2\x80\x94 " + plan.rezzerName +
                       " is coming to resurrect them.";
            if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                DcStatusPublisher::SendAddonMessage(botAI, "CHAT\t" + line);
            LOG_INFO("playerbots.dungeonclear",
                     "[DC:{}] post-combat rez: holding the run \xe2\x80\x94 {}",
                     owner->GetName(), line);
        }

        return plan;
    }
}

namespace DcRezRecovery
{
    bool Enabled(Player* bot)
    {
        return DcSettings::GetBool(bot, "PostCombatRez");
    }

    Plan Evaluate(Player* bot)
    {
        return EvaluateImpl(bot, /*mutate*/ true);
    }

    bool IsPending(Player* leaderTank)
    {
        if (!leaderTank || !Enabled(leaderTank))
            return false;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(leaderTank);
        if (!ai)
            return false;
        DcRunState const& run = DcRun::Of(ai);
        if (!run.enabled || run.paused)
            return false;

        if (leaderTank->isDead())
            return true;
        Group* group = leaderTank->GetGroup();
        if (!group)
            return false;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member->GetMapId() != leaderTank->GetMapId())
                continue;
            if (member->isDead())
                return true;
        }
        return false;
    }

    bool CanRecover(Player* bot)
    {
        if (!bot || !Enabled(bot))
            return false;
        Group* group = bot->GetGroup();
        if (!group)
            return bot->IsAlive() && IsRezClass(bot);
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member->GetMapId() != bot->GetMapId())
                continue;
            if (member->IsAlive() && IsRezClass(member))
                return true;
        }
        return false;
    }

    std::string DescribeWait(Player* bot)
    {
        Plan const plan = EvaluateImpl(bot, /*mutate*/ false);
        if (plan.verdict.outcome != DcRezDecision::Outcome::Hold)
            return "";
        if (plan.verdict.reason == DcRezDecision::Reason::WaitingOnHuman)
            return "Waiting for you to resurrect " + plan.targetName + ".";
        return plan.rezzerName + " is coming to resurrect " + plan.targetName + ".";
    }
}
