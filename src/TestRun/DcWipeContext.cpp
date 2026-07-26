/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TestRun/DcWipeContext.h"

namespace DcTestRun
{
    Engagement UpdateEngagement(Engagement const& prev, EngagementSample const& sample)
    {
        if (!sample.bossName.empty())
            return {true, sample.bossEntry, sample.bossName};

        if (!sample.trashName.empty())
            return {false, sample.trashEntry, sample.trashName};

        // Survivors, nothing on them: the fight is over and won.
        if (sample.anyAlive && !sample.anyAliveInCombat)
            return {};

        // Either the party is down (nobody left to sample) or it is in combat
        // with something that resolved to no creature this tick. Both want the
        // last real engagement kept.
        return prev;
    }
}
