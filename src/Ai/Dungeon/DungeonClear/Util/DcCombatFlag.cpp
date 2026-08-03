/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcCombatFlag.h"

#include "Ai/Dungeon/DungeonClear/DcApproachState.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearMath.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearTuning.h"

#include "AiObjectContext.h"
#include "Group.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Timer.h"

namespace DcCombatFlag
{
    bool IsEngaged(Player* p)
    {
        return p && p->IsAlive() && (p->GetVictim() || !p->getAttackers().empty());
    }

    bool AnyPartyEngagement(Player* bot)
    {
        if (!bot)
            return false;

        if (IsEngaged(bot))
            return true;
        Group* group = bot->GetGroup();
        if (!group)
            return false;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->GetMapId() == bot->GetMapId() &&
                IsEngaged(member))
                return true;
        }
        return false;
    }

    bool AnyPartyCombatFlag(Player* bot)
    {
        if (!bot)
            return false;
        if (bot->IsInCombat())
            return true;
        Group* group = bot->GetGroup();
        if (!group)
            return false;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && member->IsAlive() &&
                member->GetMapId() == bot->GetMapId() && member->IsInCombat())
                return true;
        }
        return false;
    }

    bool MayDrive(Player* bot, AiObjectContext* context)
    {
        if (!bot || !context)
            return false;
        DcApproachState& appr = context->GetValue<DcApproachState&>(DcKey::ApproachState)->Get();
        return DungeonClearMath::MayDriveWhileFlagged(
            bot->IsInCombat(), AnyPartyEngagement(bot), getMSTime(),
            DC_FLAGGED_NO_ENGAGE_GRACE_MS, appr.flaggedNoEngageSinceMs);
    }

    bool IsPhantomFlag(Player* bot, AiObjectContext* context)
    {
        if (!bot || !context)
            return false;
        // PARTY-wide flag, unlike MayDrive's own-flag test: the between-pulls gate
        // waits for every member to top up, so one member an aura holds in combat
        // is enough to make the wait unsatisfiable — and the tank commonly drops
        // combat a second or two before its followers do.
        //
        // The same kernel and the same grace, on its own latch (see
        // DcApproachState). The grace is what stops this firing in the window at
        // the end of every ordinary fight, where the party is still flagged and
        // nothing is engaged any more — waiving the floors there would send the
        // tank to the next pull instead of drinking. Feeding the flag THROUGH the
        // kernel (rather than early-returning on it) is deliberate: an unflagged
        // tick must clear the streak, or a later phantom flag would inherit a
        // stale timestamp and skip its grace entirely.
        DcApproachState& appr = context->GetValue<DcApproachState&>(DcKey::ApproachState)->Get();
        bool const flagged = AnyPartyCombatFlag(bot);
        return DungeonClearMath::MayDriveWhileFlagged(
                   flagged, AnyPartyEngagement(bot), getMSTime(),
                   DC_FLAGGED_NO_ENGAGE_GRACE_MS, appr.partyFlaggedNoEngageSinceMs) &&
               flagged;
    }
}
