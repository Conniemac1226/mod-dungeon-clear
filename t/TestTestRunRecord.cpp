/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <cstdlib>
#include <string>

#include "TestRun/DcTestRunRecord.h"

using DcTestRunRecord::EscapeJson;
using DcTestRunRecord::Record;
using DcTestRunRecord::StatusEntry;
using DcTestRunRecord::ToJsonl;

namespace
{
    Record SampleRecord()
    {
        Record r;
        r.runId = "tr-test-1";
        r.planId = "tp-test-1";
        r.dungeon = "deadmines";
        r.dungeonName = "The Deadmines";
        r.mapId = 36;
        r.instanceId = 174;
        r.level = 18;
        r.comp.push_back({"Botname", "warrior", "prot pve", "tank", 1234, 18});
        r.startedAtMs = 1000;
        r.endedAtMs = 813000;
        r.durationS = 812;
        r.result = "success";
        r.disableReason = "All bosses cleared!";
        r.bossesTotal = 6;
        r.bossesKilled = 6;
        r.bossTimeline.push_back({142, 644, "Rhahk'Zor", "mask"});
        r.statusTimeline.push_back({3, "moving", "En route"});
        r.pauses.push_back({222, "a closed door is blocking the path"});
        return r;
    }

    // Count occurrences of a substring.
    std::size_t Count(std::string const& hay, std::string const& needle)
    {
        std::size_t n = 0;
        for (std::size_t at = hay.find(needle); at != std::string::npos;
             at = hay.find(needle, at + needle.size()))
            ++n;
        return n;
    }
}

// ---- escaping --------------------------------------------------------------------

TEST(DcTestRunRecordTest, EscapesQuotesBackslashesAndControlChars)
{
    EXPECT_EQ(EscapeJson("plain"), "plain");
    EXPECT_EQ(EscapeJson("say \"hi\""), "say \\\"hi\\\"");
    EXPECT_EQ(EscapeJson("a\\b"), "a\\\\b");
    EXPECT_EQ(EscapeJson("line\nbreak"), "line\\nbreak");
    // Split literal: a plain "\x01b" would greedily parse as hex 0x1B.
    EXPECT_EQ(EscapeJson(std::string("nul\x01" "byte")), "nul\\u0001byte");
}

// ---- serializer shape ------------------------------------------------------------

TEST(DcTestRunRecordTest, SerializesKnownFields)
{
    std::string const line = ToJsonl(SampleRecord());
    EXPECT_EQ(line.front(), '{');
    EXPECT_EQ(line.back(), '}');
    EXPECT_NE(line.find("\"runId\":\"tr-test-1\""), std::string::npos);
    EXPECT_NE(line.find("\"planId\":\"tp-test-1\""), std::string::npos);
    EXPECT_NE(line.find("\"dungeon\":\"deadmines\""), std::string::npos);
    EXPECT_NE(line.find("\"mapId\":36"), std::string::npos);
    EXPECT_NE(line.find("\"result\":\"success\""), std::string::npos);
    EXPECT_NE(line.find("\"disableReason\":\"All bosses cleared!\""), std::string::npos);
    EXPECT_NE(line.find("\"name\":\"Rhahk'Zor\""), std::string::npos);
    EXPECT_NE(line.find("\"bossesKilled\":6"), std::string::npos);
    EXPECT_NE(line.find("\"heroic\":false"), std::string::npos);
    // One line — the sink is JSONL.
    EXPECT_EQ(line.find('\n'), std::string::npos);
}

// ---- wipe post-mortem ------------------------------------------------------------

TEST(DcTestRunRecordTest, WipeOnABossSerializesTheBoss)
{
    Record r = SampleRecord();
    r.result = "wipe";
    r.failReason = "party wiped on Ghaz'an (last standing: Botname)";
    r.wipeOnBoss = true;
    r.wipeOpponentEntry = 18105;
    r.wipeOpponent = "Ghaz'an";

    std::string const line = ToJsonl(r);
    EXPECT_NE(line.find("\"wipeOnBoss\":true"), std::string::npos);
    EXPECT_NE(line.find("\"wipeOpponentEntry\":18105"), std::string::npos);
    EXPECT_NE(line.find("\"wipeOpponent\":\"Ghaz'an\""), std::string::npos);
}

TEST(DcTestRunRecordTest, WipeToTrashKeepsTheBossFlagFalse)
{
    Record r = SampleRecord();
    r.result = "wipe";
    r.wipeOnBoss = false;
    r.wipeOpponentEntry = 17826;
    r.wipeOpponent = "Bog Giant";

    std::string const line = ToJsonl(r);
    EXPECT_NE(line.find("\"wipeOnBoss\":false"), std::string::npos);
    EXPECT_NE(line.find("\"wipeOpponent\":\"Bog Giant\""), std::string::npos);
}

TEST(DcTestRunRecordTest, WipeFieldsAreAlwaysPresentAndDefaultEmpty)
{
    // The dashboard reads them unconditionally; a run nobody died in must still
    // carry the keys, empty.
    std::string const line = ToJsonl(SampleRecord());
    EXPECT_NE(line.find("\"wipeOnBoss\":false"), std::string::npos);
    EXPECT_NE(line.find("\"wipeOpponentEntry\":0"), std::string::npos);
    EXPECT_NE(line.find("\"wipeOpponent\":\"\""), std::string::npos);
}

TEST(DcTestRunRecordTest, HeroicSerializesTrue)
{
    Record r = SampleRecord();
    r.heroic = true;
    EXPECT_NE(ToJsonl(r).find("\"heroic\":true"), std::string::npos);
}

TEST(DcTestRunRecordTest, FreeTextReasonsAreEscapedInPlace)
{
    Record r = SampleRecord();
    r.failReason = "boss said \"no\"\nand left";
    std::string const line = ToJsonl(r);
    EXPECT_NE(line.find("\"failReason\":\"boss said \\\"no\\\"\\nand left\""),
              std::string::npos);
    EXPECT_EQ(line.find('\n'), std::string::npos);
}

// ---- status-timeline cap ---------------------------------------------------------

TEST(DcTestRunRecordTest, ShortStatusTimelineIsKeptWhole)
{
    Record r = SampleRecord();
    r.statusTimeline.clear();
    for (std::uint32_t i = 0; i < 100; ++i)
        r.statusTimeline.push_back({i, "moving", ""});
    std::string const line = ToJsonl(r);
    EXPECT_EQ(Count(line, "\"state\":\"moving\""), 100u);
    EXPECT_EQ(line.find("elided"), std::string::npos);
}

TEST(DcTestRunRecordTest, LongStatusTimelineKeepsHeadAndTail)
{
    Record r = SampleRecord();
    r.statusTimeline.clear();
    for (std::uint32_t i = 0; i < 500; ++i)
        r.statusTimeline.push_back({i, "moving", ""});
    std::string const line = ToJsonl(r);
    // 50 head + 150 tail survive; the elision marker declares the rest.
    EXPECT_EQ(Count(line, "\"state\":\"moving\""),
              DcTestRunRecord::kStatusHead + DcTestRunRecord::kStatusTail);
    EXPECT_NE(line.find("300 transitions elided"), std::string::npos);
    // Tail is the LAST entries: t=499 present, a mid value dropped.
    EXPECT_NE(line.find("{\"t\":499,"), std::string::npos);
    EXPECT_EQ(line.find("{\"t\":200,"), std::string::npos);
}

// ---- capture path ----------------------------------------------------------------

TEST(DcTestRunRecordTest, DefaultCapturePath)
{
    // The env override is exercised operationally; here just pin the default.
    if (!std::getenv("DC_TESTRUNS_FILE"))
        EXPECT_EQ(DcTestRunRecord::CapturePath(), "dc_testruns.jsonl");
}
