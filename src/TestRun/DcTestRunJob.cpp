/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TestRun/DcTestRunJob.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <optional>
#include <set>

#include "CharacterCache.h"
#include "Chat.h"
#include "Creature.h"
#include "Group.h"
#include "GroupMgr.h"
#include "InstanceSaveMgr.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringFormat.h"

#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "PlayerbotGuildMgr.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"
#include "Random.h"

#include "DcStrategyGate.h"
#include "Ai/Dungeon/DungeonClear/Action/DcActionShared.h"
#include "Ai/Dungeon/DungeonClear/Data/DungeonBossInfo.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcTargeting.h"
#include "TestRun/DcDiagSnapshot.h"
#include "TestRun/DcTestComp.h"

namespace
{
    // Stage timeouts: a setup stage overrunning these is itself the failure.
    constexpr uint32 SPAWN_TIMEOUT_MS = 60 * 1000;
    constexpr uint32 PROVISION_TIMEOUT_MS = 60 * 1000;
    constexpr uint32 GROUP_TIMEOUT_MS = 30 * 1000;
    constexpr uint32 TELEPORT_TIMEOUT_MS = 30 * 1000;
    constexpr uint32 START_TIMEOUT_MS = 20 * 1000;

    constexpr uint32 MONITOR_STEP_MS = 1000;

    // How much closer the party must get to its target for the sample to count
    // as ground gained. Above the idle jitter of a bot settling on a spot, so a
    // parked tank drifting a few centimetres can't hold the watchdog open
    // forever; far below any real leg of travel.
    constexpr float DC_TESTRUN_PROGRESS_EPSILON_YD = 1.0f;

    uint64 NowUnixMs()
    {
        return static_cast<uint64>(std::time(nullptr)) * 1000;
    }

    std::string MakeRunId()
    {
        static uint32 counter = 0;
        std::time_t const now = std::time(nullptr);
        std::tm tmBuf{};
        localtime_r(&now, &tmBuf);
        char buf[32];
        std::strftime(buf, sizeof(buf), "tr-%Y%m%d-%H%M%S", &tmBuf);
        return std::string(buf) + "-" + std::to_string(++counter);
    }

    char const* ClassToken(uint8 classId)
    {
        switch (classId)
        {
            case 1:  return "warrior";
            case 2:  return "paladin";
            case 3:  return "hunter";
            case 4:  return "rogue";
            case 5:  return "priest";
            case 6:  return "deathknight";
            case 7:  return "shaman";
            case 8:  return "mage";
            case 9:  return "warlock";
            case 11: return "druid";
        }
        return "unknown";
    }

    // Premade-spec template index for the wanted spec name — exact match
    // first, then substring fallback ("prot" catches a renamed "prot pve").
    // -1 when the class has no matching template.
    int ResolveSpecNo(uint8 classId, char const* exact, char const* fallback,
                      std::string* pickedName)
    {
        for (int pass = 0; pass < 2; ++pass)
            for (int i = 0; i < MAX_SPECNO; ++i)
            {
                std::string const& name = sPlayerbotAIConfig.premadeSpecName[classId][i];
                if (name.empty())
                    break;
                bool const hit = pass == 0 ? name == exact
                                           : name.find(fallback) != std::string::npos;
                if (hit)
                {
                    if (pickedName)
                        *pickedName = name;
                    return i;
                }
            }
        return -1;
    }
}

char const* DcTestRunJob::StageName(Stage s)
{
    switch (s)
    {
        case Stage::SpawningBots: return "spawning_bots";
        case Stage::Provisioning: return "provisioning";
        case Stage::Grouping:     return "grouping";
        case Stage::Teleporting:  return "teleporting";
        case Stage::Starting:     return "starting";
        case Stage::Monitoring:   return "monitoring";
        case Stage::TearingDown:  return "tearing_down";
    }
    return "?";
}

void DcTestRunJob::EnterStage(Stage s)
{
    _stage.store(s);
    _stageMs = 0;
}

Player* DcTestRunJob::FindGm() const
{
    // FindConnectedPlayer, NOT FindPlayer: the GM teleporting into the
    // instance to watch sits on a loading screen for a few ticks — not in
    // world, but very much still logged in. FindPlayer() returned null there,
    // and the liveness check below read it as a logout and aborted every run
    // the moment the GM zoned in. Session-based lookup survives the loading
    // screen; a real logout still goes null as soon as the session closes.
    return ObjectAccessor::FindConnectedPlayer(_gmGuid);
}

Player* DcTestRunJob::FindTank() const
{
    return ObjectAccessor::FindPlayer(_tankGuid);
}

std::unique_ptr<DcTestRunJob> DcTestRunJob::Create(Player* gm, DcTestDungeonRegistry::Row const& row,
                                                   uint32 levelOverride, uint32 seed, bool heroic,
                                                   std::unordered_set<ObjectGuid> const& reservedGuids,
                                                   std::string const& planId, std::string* err)
{
    // Caller (DcTestRunManager::Start) has already validated the registry row
    // (including that heroic is only requested where heroicLevel is set) and
    // that gm has a playerbot manager.
    std::unique_ptr<DcTestRunJob> job(new DcTestRunJob());

    job->_dungeonToken = row.token;
    job->_mapId = row.mapId;
    job->_x = row.x;
    job->_y = row.y;
    job->_z = row.z;
    job->_o = row.o;
    job->_heroic = heroic;
    job->_level = levelOverride ? std::min<uint32>(levelOverride, 80u)
                                : (heroic ? row.heroicLevel : row.recommendedLevel);
    job->_gmGuid = gm->GetGUID();

    // seed 0 = "roll one" — pick a nonzero seed so the comp varies per run yet
    // is recorded for exact replay via `.dc test start <d> seed=N`.
    if (seed == 0)
        seed = rand32() | 1u;

    job->_limits.pauseGraceMs = DcSettings::GetUInt(ObjectGuid::Empty, "TestRun.PauseGraceS") * 1000;
    job->_limits.stallGraceMs = DcSettings::GetUInt(ObjectGuid::Empty, "TestRun.StallGraceS") * 1000;
    job->_limits.noProgressMs = DcSettings::GetUInt(ObjectGuid::Empty, "TestRun.NoProgressS") * 1000;
    job->_limits.overallTimeoutMs = DcSettings::GetUInt(ObjectGuid::Empty, "TestRun.OverallTimeoutS") * 1000;

    job->_record = DcTestRunRecord::Record{};
    job->_record.runId = MakeRunId();
    job->_record.planId = planId;
    job->_record.dungeon = row.token;
    job->_record.dungeonName = row.name;
    job->_record.wing = row.wing;
    job->_record.mapId = row.mapId;
    job->_record.level = job->_level;
    job->_record.heroic = heroic;
    job->_record.compSeed = seed;
    job->_record.startedAtMs = NowUnixMs();
    job->_record.pauseGraceS = job->_limits.pauseGraceMs / 1000;
    job->_record.stallGraceS = job->_limits.stallGraceMs / 1000;
    job->_record.noProgressS = job->_limits.noProgressMs / 1000;
    job->_record.overallS = job->_limits.overallTimeoutMs / 1000;

    std::array<DcTestComp::Slot, DcTestComp::kPartySize> const comp = DcTestComp::BuildComp(seed);
    for (DcTestComp::Slot const& c : comp)
    {
        Slot s;
        s.classId = c.classId;
        s.specName = c.specName;
        s.fallbackSpec = c.fallbackSpec;
        s.role = c.role;
        job->_slots.push_back(s);
    }

    PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(gm);
    bool const isAlliance = gm->GetTeamId(true) == TEAM_ALLIANCE;

    // Claim one offline addclass-pool character of `classId` (the `addclass`
    // command's own selection rules), or Empty if none is free.
    auto claim = [&](uint8 classId) -> ObjectGuid
    {
        auto const& pool = sRandomPlayerbotMgr.addclassCache[
            RandomPlayerbotMgr::GetTeamClassIdx(isAlliance, classId)];
        for (ObjectGuid const& guid : pool)
        {
            // Skip guids already claimed by this job's earlier slots and by any
            // other live run (cross-run reservation set, world-thread only).
            bool taken = false;
            for (Slot const& other : job->_slots)
                if (other.guid == guid)
                    taken = true;
            if (taken)
                continue;
            if (reservedGuids.find(guid) != reservedGuids.end())
                continue;
            // (No botLoading check — it is protected on PlayerbotHolder;
            // AddPlayerBot itself no-ops on an in-flight load, and a char
            // loading for someone else just times the spawn stage out.)
            if (ObjectAccessor::FindConnectedPlayer(guid))
                continue;
            uint32 const guildId = sCharacterCache->GetCharacterGuildIdByGuid(guid);
            if (guildId && PlayerbotGuildMgr::instance().IsRealGuild(guildId))
                continue;
            return guid;
        }
        return ObjectGuid::Empty;
    };

    // Fill each slot with its drawn class, then start its async login. When the
    // drawn class has no free pool character, substitute another class of the
    // same role that does (and isn't already in the party) rather than fail —
    // randomisation shouldn't abort a run just because one class is un-seeded.
    // Only a role with no fillable class at all is fatal.
    std::set<uint8> usedClasses;
    for (Slot& slot : job->_slots)
    {
        ObjectGuid guid = claim(slot.classId);
        if (!guid)
        {
            for (DcTestComp::Slot const& alt : DcTestComp::RolePool(slot.role))
            {
                if (alt.classId == slot.classId || usedClasses.count(alt.classId))
                    continue;
                guid = claim(alt.classId);
                if (guid)
                {
                    LOG_INFO("playerbots.dungeonclear",
                             "TESTRUN {} substituting {} for unavailable {} ({})",
                             job->_record.runId, ClassToken(alt.classId),
                             ClassToken(slot.classId), slot.role);
                    slot.classId = alt.classId;
                    slot.specName = alt.specName;
                    slot.fallbackSpec = alt.fallbackSpec;
                    break;
                }
            }
        }
        if (!guid)
        {
            if (err)
                *err = std::string("no available ") + slot.role +
                       " class in the addclass pool — pre-seed with `.playerbots addclass`";
            return nullptr;
        }
        slot.guid = guid;
        usedClasses.insert(slot.classId);
        mgr->AddPlayerBot(slot.guid, gm->GetSession()->GetAccountId());
    }

    LOG_INFO("playerbots.dungeonclear",
             "TESTRUN START {} dungeon={} map={} level={} heroic={} seed={} gm={}",
             job->_record.runId, job->_record.dungeon, job->_mapId, job->_level,
             heroic ? 1 : 0, seed, gm->GetName());

    job->EnterStage(Stage::SpawningBots);
    return job;
}

void DcTestRunJob::RequestAbort(std::string const& reason)
{
    std::lock_guard<std::mutex> lock(_obsMutex);
    _abortRequested = true;
    _abortReason = reason;
}

void DcTestRunJob::AbortSetup(std::string const& reason)
{
    FailSetup(reason);
}

std::string DcTestRunJob::StatusLine() const
{
    Stage const stage = _stage.load();
    std::string out = _record.runId + " " + _record.dungeon + (_heroic ? " (heroic)" : "") +
                      " [" + StageName(stage) + "] elapsed " + std::to_string(_totalMs / 1000) + "s";
    if (stage == Stage::Monitoring)
    {
        out += ", bosses " + std::to_string(_record.bossesKilled) + "/" +
               std::to_string(_record.bossesTotal);
        std::lock_guard<std::mutex> lock(_obsMutex);
        if (!_lastStatusState.empty())
            out += ", state " + _lastStatusState;
    }
    return out;
}

DcTestRunLive::RunSnapshot DcTestRunJob::Snapshot() const
{
    DcTestRunLive::RunSnapshot s;
    s.runId = _record.runId;
    s.planId = _record.planId;
    s.dungeon = _record.dungeon;
    s.dungeonName = _record.dungeonName;
    s.stage = StageName(_stage.load());
    s.level = _level;
    s.heroic = _heroic;
    s.elapsedS = _totalMs / 1000;
    s.bossesKilled = _record.bossesKilled;
    s.bossesTotal = _record.bossesTotal;

    // Live positions for the dashboard map overlay. Snapshot() is called from
    // the manager's world-thread tick (WriteLiveStatus), so resolving guids to
    // players and reading their transform is safe here. The run's mapId is the
    // tank's (all members share one instance); a member still loading resolves
    // to null and is simply omitted this heartbeat.
    for (Slot const& slot : _slots)
    {
        if (!slot.guid)
            continue;
        Player* p = ObjectAccessor::FindPlayer(slot.guid);
        if (!p)
            continue;
        if (s.mapId < 0)
            s.mapId = static_cast<std::int32_t>(p->GetMapId());
        DcTestRunLive::BotPos bp;
        bp.role = slot.role;
        // The slot only knows class/spec/role; the name belongs to the char the
        // pool handed out, so it has to come off the resolved player.
        bp.name = p->GetName();
        bp.classId = slot.classId;
        bp.x = p->GetPositionX();
        bp.y = p->GetPositionY();
        bp.z = p->GetPositionZ();
        bp.alive = p->IsAlive();
        bp.hp = static_cast<std::uint8_t>(bp.alive ? p->GetHealthPct() : 0.f);
        // Gate on the mana POOL, not on getPowerType(): a druid shifted into
        // bear/cat reports RAGE/ENERGY while still holding the mana it has to
        // shift back and drink for. Keying off the active power type would blank
        // the bar for exactly the member whose mana the run is waiting on.
        // Non-mana classes have maxMana 0 and stay at the -1 "no bar" default.
        if (bp.alive && p->GetMaxPower(POWER_MANA) > 0)
            bp.mp = static_cast<std::int16_t>(p->GetPowerPct(POWER_MANA));
        bp.inCombat = p->IsInCombat();
        if (bp.inCombat)
            s.inCombat = true;
        s.bots.push_back(std::move(bp));
    }

    // Why the run is sitting still, read live off the tank rather than from the
    // status timeline: the timeline only records state CHANGES, so a run that
    // has been wedged in one state for ten minutes has nothing recent in it.
    if (Player* tank = FindTank())
        if (PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank))
        {
            AiObjectContext* ctx = tankAI->GetAiObjectContext();
            s.stall = ctx->GetValue<std::string&>(DcKey::StallReason)->Get();
            std::optional<DungeonBossInfo> const next =
                ctx->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();
            if (next.has_value())
                s.bossName = next->name;
        }

    // Not under _obsMutex: the watchdog writes _sinceProgressMs outside it too,
    // and both that write and this read happen on the world thread.
    s.sinceProgressS = _sinceProgressMs / 1000;

    // A wipe holds the run for the grace window (a battle rez can still save
    // it), during which the live card would otherwise show a party doing
    // nothing at all. Name the killer as soon as the harness sees the wipe
    // rather than only in the finished record.
    if (_wipedForMs > 0)
    {
        s.wiped = true;
        s.wipeOnBoss = _engaged.isBoss;
        s.wipeOpponent = _engaged.name;
    }

    std::lock_guard<std::mutex> lock(_obsMutex);
    s.state = _lastStatusState;
    std::vector<DcTestRunRecord::StatusEntry> const& st = _record.statusTimeline;
    std::size_t const from = st.size() > 8 ? st.size() - 8 : 0;
    for (std::size_t i = from; i < st.size(); ++i)
        s.recent.push_back({st[i].t, st[i].state, st[i].detail});
    return s;
}

std::vector<ObjectGuid> DcTestRunJob::BotGuids() const
{
    std::vector<ObjectGuid> out;
    out.reserve(_slots.size());
    for (Slot const& slot : _slots)
        if (slot.guid)
            out.push_back(slot.guid);
    return out;
}

void DcTestRunJob::Tick(uint32 diff, bool& provisionBudget)
{
    Stage const stage = _stage.load();
    if (stage == Stage::TearingDown || _done)
        return;

    _stageMs += diff;
    _totalMs += diff;

    // The GM's session anchors the bots (their logout logs the party out);
    // without it the run cannot finish cleanly whatever stage it is in. One
    // GM's logout kills only their runs — the check is per-job.
    if (!FindGm())
    {
        if (stage == Stage::Monitoring)
            Finish(DcTestRun::Verdict::FailAborted, "GM logged out mid-run");
        else
            FailSetup("GM logged out during setup");
        return;
    }

    switch (stage)
    {
        case Stage::SpawningBots:
            TickSpawning();
            break;
        case Stage::Provisioning:
            TickProvisioning(provisionBudget);
            break;
        case Stage::Grouping:
            TickGrouping();
            break;
        case Stage::Teleporting:
            TickTeleporting();
            break;
        case Stage::Starting:
            TickStarting();
            break;
        case Stage::Monitoring:
            _monitorMs += diff;
            _monitorAccumMs += diff;
            if (_monitorAccumMs >= MONITOR_STEP_MS)
            {
                uint32 const dt = _monitorAccumMs;
                _monitorAccumMs = 0;
                TickMonitoring(dt);
            }
            break;
        default:
            break;
    }
}

void DcTestRunJob::TickSpawning()
{
    bool allIn = true;
    for (Slot const& slot : _slots)
    {
        Player* bot = ObjectAccessor::FindPlayer(slot.guid);
        if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
        {
            allIn = false;
            break;
        }
    }

    if (allIn)
    {
        EnterStage(Stage::Provisioning);
        return;
    }

    if (_stageMs >= SPAWN_TIMEOUT_MS)
        FailSetup("bots did not finish logging in (addclass pool empty, "
                  "maxAddedBots cap, or login failure — see server log)");
}

void DcTestRunJob::TickProvisioning(bool& provisionBudget)
{
    if (_stageMs >= PROVISION_TIMEOUT_MS)
    {
        FailSetup("provisioning timed out");
        return;
    }

    if (_provisionIdx >= _slots.size())
    {
        EnterStage(Stage::Grouping);
        return;
    }

    Slot& slot = _slots[_provisionIdx];
    Player* bot = ObjectAccessor::FindPlayer(slot.guid);
    PlayerbotAI* botAI = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
    if (!bot || !bot->IsInWorld() || !botAI)
        return;  // transient (mid world-add/teleport) — retry; stage timeout bounds it

    std::string pickedSpec;
    int const specNo = ResolveSpecNo(slot.classId, slot.specName, slot.fallbackSpec, &pickedSpec);
    if (specNo < 0 && (slot.role == std::string("tank") || slot.role == std::string("heal")))
    {
        // A random-rolled tank/healer spec would invalidate the whole run.
        FailSetup(std::string("no premade spec template matching '") + slot.specName +
                  "' for " + ClassToken(slot.classId) +
                  " (AiPlayerbot.PremadeSpecName.*) — cannot force the " + slot.role);
        return;
    }

    // One factory roll per world tick across ALL runs — Randomize is heavyweight
    // (full gear/spell/talent roll) and several in one tick would be a visible
    // stall. If another run already spent this tick's budget, retry next tick;
    // the stage timeout bounds the wait.
    if (!provisionBudget)
        return;
    provisionBudget = false;

    // Full roll at the target level first (Randomize includes GiveLevel and
    // re-picks talents), then force the role spec and re-gear for it — the
    // same sequence the `talents spec` chat command uses.
    PlayerbotFactory factory(bot, _level, ITEM_QUALITY_EPIC);
    factory.Randomize(false);
    if (specNo >= 0)
    {
        PlayerbotFactory::InitTalentsBySpecNo(bot, specNo, true);
        factory.InitEquipment(false, true);
        factory.InitGlyphs(false);

        // Gear first, enchants/gems second — the order the `autogear` then
        // `maintenance` chat commands run in. Randomize() already ends with an
        // ApplyEnchantAndGemsNew() pass, but the spec re-gear above swaps those
        // enchanted/gemmed items out for freshly rolled bare ones, so without
        // this second pass every spec-forced bot (i.e. every tank and healer in
        // a test run) fights with no enchants and empty sockets. Cheap relative
        // to Randomize, and it only touches what is currently equipped.
        if (bot->GetLevel() >= sPlayerbotAIConfig.minEnchantingBotLevel)
            factory.ApplyEnchantAndGemsNew();
    }
    if (bot->getClass() == CLASS_HUNTER)
    {
        factory.InitPet();
        factory.InitAmmo();
    }
    botAI->ResetStrategies();

    DcTestRunRecord::CompEntry entry;
    entry.name = bot->GetName();
    entry.className = ClassToken(slot.classId);
    entry.spec = specNo >= 0 ? pickedSpec : "(random)";
    entry.role = slot.role;
    entry.guid = slot.guid.GetRawValue();
    entry.level = bot->GetLevel();
    _record.comp.push_back(entry);

    LOG_INFO("playerbots.dungeonclear", "TESTRUN {} provisioned {} ({} {}, level {})",
             _record.runId, bot->GetName(), entry.spec, entry.role, entry.level);

    slot.provisioned = true;
    ++_provisionIdx;
}

void DcTestRunJob::TickGrouping()
{
    if (!_groupFormed)
    {
        Player* tank = ObjectAccessor::FindPlayer(_slots[0].guid);
        if (!tank)
        {
            if (_stageMs >= GROUP_TIMEOUT_MS)
                FailSetup("tank vanished before grouping");
            return;
        }

        for (Slot const& slot : _slots)
            if (Player* bot = ObjectAccessor::FindPlayer(slot.guid))
                if (bot->GetGroup())
                    bot->RemoveFromGroup();

        // Direct group formation (the LFGMgr pattern) — no invite/accept
        // packet round-trips, and Create() makes the tank the leader.
        Group* group = new Group();
        if (!group->Create(tank))
        {
            delete group;
            FailSetup("group creation failed");
            return;
        }
        sGroupMgr->AddGroup(group);
        for (std::size_t i = 1; i < _slots.size(); ++i)
        {
            Player* bot = ObjectAccessor::FindPlayer(_slots[i].guid);
            if (!bot || !group->AddMember(bot))
            {
                FailSetup(std::string("could not add ") + ClassToken(_slots[i].classId) +
                          " to the group");
                return;
            }
        }
        group->SetDungeonDifficulty(_heroic ? DUNGEON_DIFFICULTY_HEROIC
                                            : DUNGEON_DIFFICULTY_NORMAL);
        _tankGuid = tank->GetGUID();
        _groupFormed = true;
    }

    Player* tank = FindTank();
    Group* group = tank ? tank->GetGroup() : nullptr;
    if (group && group->GetMembersCount() == _slots.size() && group->IsLeader(_tankGuid))
    {
        // Keep the GM (a real human Player) as each bot's playerbots MASTER, and
        // strip the stock follow-master strategy instead of nulling the master.
        //
        // Why not masterless: HasRealPlayerMaster() gates the whole "a human is
        // driving me" fast path in stock playerbots. With no real-player master,
        // GetReactDelay() returns base*10 (1000ms vs 100ms) out of combat, so the
        // party thinks on a 1s beat between packs — test runs looked far slower
        // and sloppier than a real human-led run. HasRealPlayerMaster() has no
        // map/distance/visibility gate (it only checks the master pointer is a
        // non-bot Player), so keeping the GM as master holds the fast path even
        // though the GM is invisible and outside the instance — the test now
        // mirrors a real run in every master-gated respect (react delay, AoE
        // avoidance, wait-for-attack), which is the point of a regression harness.
        //
        // The only reason the old code went masterless was to stop stock
        // follow-master from dragging the party toward the GM. Follow-master is
        // just the "follow" strategy, so we remove it outright here. DC's own
        // follow-tank redirect (a DcMovementAction, not a FollowAction) drives
        // the followers, and DungeonClearMultiplier additionally zeroes
        // FollowAction for every active-run member once the run is enabled —
        // this strip covers the pre-enable / paused window. A GM master sticks:
        // FindNewMaster() only re-resolves a null or bot master.
        Player* gm = FindGm();
        for (Slot const& slot : _slots)
            if (Player* bot = ObjectAccessor::FindPlayer(slot.guid))
                if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                {
                    if (gm)
                        botAI->SetMaster(gm);
                    botAI->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);
                    botAI->ChangeStrategy("-follow", BOT_STATE_COMBAT);
                }

        EnterStage(Stage::Teleporting);
        return;
    }

    if (_stageMs >= GROUP_TIMEOUT_MS)
        FailSetup("group did not form");
}

void DcTestRunJob::TickTeleporting()
{
    if (!_teleportIssued)
    {
        for (Slot const& slot : _slots)
        {
            Player* bot = ObjectAccessor::FindPlayer(slot.guid);
            if (!bot)
            {
                if (_stageMs >= TELEPORT_TIMEOUT_MS)
                    FailSetup("bot vanished before teleport");
                return;  // transient — retry next tick
            }
        }
        // Heroic 5-man kills create PERMANENT saves (daily reset). A recycled
        // pool bot still bound to a cleared heroic instance of this map would
        // teleport into it and trip the stale-instance guard in TickStarting —
        // shed any leftover heroic bind first so a fresh instance is created.
        // (Normal 5-man saves are non-permanent and reset when the map empties,
        // so they need no such hygiene.)
        if (_heroic)
            for (Slot const& slot : _slots)
                sInstanceSaveMgr->PlayerUnbindInstance(slot.guid, _mapId,
                                                       DUNGEON_DIFFICULTY_HEROIC,
                                                       /*deleteFromDB*/ true);

        for (Slot const& slot : _slots)
            if (Player* bot = ObjectAccessor::FindPlayer(slot.guid))
                bot->TeleportTo(_mapId, _x, _y, _z, _o);
        _teleportIssued = true;
    }

    bool allThere = true;
    for (Slot const& slot : _slots)
    {
        Player* bot = ObjectAccessor::FindPlayer(slot.guid);
        if (!bot || bot->GetMapId() != _mapId || !bot->IsInWorld() || bot->IsBeingTeleported())
        {
            allThere = false;
            break;
        }
    }

    if (allThere)
    {
        EnterStage(Stage::Starting);
        return;
    }

    if (_stageMs >= TELEPORT_TIMEOUT_MS)
        FailSetup("party did not arrive at the dungeon entrance");
}

void DcTestRunJob::TickStarting()
{
    Player* tank = FindTank();
    PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
    if (!tank || !tankAI)
    {
        if (_stageMs >= START_TIMEOUT_MS)
            FailSetup("tank vanished before start");
        return;
    }

    // Freshly-teleported bots may not have the DC strategies installed yet
    // (contexts register on world ticks) — re-assert every tick; idempotent.
    for (Slot const& slot : _slots)
        if (Player* bot = ObjectAccessor::FindPlayer(slot.guid))
            DcStrategyGate::Reconcile(bot);

    AiObjectContext* ctx = tankAI->GetAiObjectContext();

    // A reused instance with dead bosses would flash an instant (false)
    // all-clear — refuse it rather than record a fake success.
    if (InstanceScript* inst = DcTargeting::GetInstanceScript(tank))
        if (inst->GetCompletedEncounterMask() != 0)
        {
            FailSetup("stale instance: encounters already completed (mask " +
                      std::to_string(inst->GetCompletedEncounterMask()) + ")");
            return;
        }

    // The boss roster needs a tick or two to populate after the teleport;
    // retry inside the stage timeout.
    std::vector<DungeonBossInfo> const bosses =
        ctx->GetValue<std::vector<DungeonBossInfo>>(DcKey::DungeonBosses)->Get();
    if (bosses.empty())
    {
        if (_stageMs >= START_TIMEOUT_MS)
            FailSetup("no boss roster for this map");
        return;
    }

    if (!_dcOnIssued)
    {
        _record.instanceId = tank->GetMap()->GetInstanceId();
        _roster.clear();
        for (DungeonBossInfo const& b : bosses)
        {
            BossRef ref;
            ref.entry = b.entry;
            ref.encounterIndex = b.encounterIndex;
            ref.name = b.name;
            ref.isBoss = b.kind == DungeonAnchorKind::Boss;
            _roster.push_back(ref);
        }
        _record.bossesTotal = static_cast<uint32>(_roster.size());

        // The run must never wait for a human: kill the WaitAtBoss pre-pull
        // hold for this run whatever the conf says.
        DcSettings::SetOverride(_tankGuid, "WaitAtBoss", 0.0);
        _dcOnIssued = true;
    }

    // Retry `dc on` each tick until the enabled flag sticks (roster/context
    // timing) or the stage times out.
    tankAI->DoSpecificAction("dc on", Event("dc", "", FindGm()), true);
    if (DcRun::Of(ctx).enabled)
    {
        if (InstanceScript* inst = DcTargeting::GetInstanceScript(tank))
            _lastMask = inst->GetCompletedEncounterMask();
        _lastAnchors =
            ctx->GetValue<std::unordered_set<uint32>&>(DcKey::ClearedAnchors)->Get().size();
        LOG_INFO("playerbots.dungeonclear",
                 "TESTRUN {} running: instance {} with {} bosses/objectives",
                 _record.runId, _record.instanceId, _record.bossesTotal);
        EnterStage(Stage::Monitoring);
        return;
    }

    if (_stageMs >= START_TIMEOUT_MS)
        FailSetup("dc on did not take (look for 'DC command refused' in the DC log)");
}

// Gather this sample's combat picture off the party and fold it into the
// engagement latch (DcTestRun::UpdateEngagement owns the rule).
//
// Only members that are alive and on the leader's map are read: a corpse holds
// neither a victim nor an attacker list, so a party being wiped simply stops
// contributing and the last engagement seen while somebody was standing is what
// the wipe verdict reports.
//
// The run's own roster is the authority on what counts as a boss — it is what
// every other line of the report names bosses by — and the creature-side flags
// are the fallback for a summoned or phase-swapped boss the roster never listed
// (Anzu, a Tuten'kash-style event spawn).
void DcTestRunJob::TrackEngagement(Player* tank)
{
    if (!tank)
        return;

    DcTestRun::EngagementSample sample;

    auto rosterName = [this](uint32 entry) -> std::string const*
    {
        for (BossRef const& ref : _roster)
            if (ref.entry == entry && ref.isBoss)
                return &ref.name;
        return nullptr;
    };

    auto consider = [&](Unit const* u)
    {
        if (!u || !u->IsAlive())
            return;
        Creature const* creature = u->ToCreature();
        if (!creature)
            return;
        std::string const* named = rosterName(creature->GetEntry());
        if (sample.bossName.empty() &&
            (named || creature->IsDungeonBoss() || creature->isWorldBoss()))
        {
            sample.bossEntry = creature->GetEntry();
            sample.bossName = named ? *named : creature->GetName();
        }
        if (sample.trashName.empty())
        {
            sample.trashEntry = creature->GetEntry();
            sample.trashName = creature->GetName();
        }
    };

    auto scan = [&](Player* member)
    {
        if (!member || !member->IsAlive() || !member->IsInWorld() ||
            member->GetMapId() != tank->GetMapId())
            return;
        sample.anyAlive = true;
        if (!member->IsInCombat())
            return;
        sample.anyAliveInCombat = true;
        // The victim covers whoever the member is swinging at; the attacker set
        // covers the mob beating on a member with no target of its own (a
        // healer, or anyone the pack peeled onto).
        consider(member->GetVictim());
        for (Unit const* attacker : member->getAttackers())
            consider(attacker);
    };

    scan(tank);
    if (Group* group = tank->GetGroup())
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource(); member && member != tank)
                scan(member);

    _engaged = DcTestRun::UpdateEngagement(_engaged, sample);
}

// Is anyone in the party on the leader's map a corpse right now? Separates a
// run that ended because people died (worth the wipe post-mortem) from one that
// ended for any other reason.
bool DcTestRunJob::AnyMemberDead(Player* tank)
{
    if (!tank)
        return false;
    if (!tank->IsAlive())
        return true;
    Group* group = tank->GetGroup();
    if (!group)
        return false;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member == tank || member->GetMapId() != tank->GetMapId())
            continue;
        if (!member->IsAlive())
            return true;
    }
    return false;
}

void DcTestRunJob::TickMonitoring(uint32 dt)
{
    DcTestRun::Observation obs;
    obs.elapsedMs = _monitorMs;
    obs.gmOnline = true;  // GM absence is handled in Tick()

    {
        std::lock_guard<std::mutex> lock(_obsMutex);
        obs.abortRequested = _abortRequested;
        obs.disableFired = _disableFired;
        obs.disableAllCleared = _disableAllCleared;
    }

    // Connected-lookup + explicit teleport tolerance: DC's own scripted
    // events teleport the party mid-run (Underbog hops, Old Hillsbrad's
    // muster) — a sample landing in that window must wait, not read the
    // leader as gone.
    Player* tank = ObjectAccessor::FindConnectedPlayer(_tankGuid);
    PlayerbotAI* tankAI = tank ? GET_PLAYERBOT_AI(tank) : nullptr;
    std::string pauseReason;
    std::string stallReason;

    if (tank && tankAI && (!tank->IsInWorld() || tank->IsBeingTeleported()))
        return;  // mid-teleport — skip this sample, timers resume next tick

    if (!tank || !tankAI)
    {
        obs.leaderMissing = true;
    }
    else
    {
        AiObjectContext* ctx = tankAI->GetAiObjectContext();
        DcRunState const& rs = DcRun::Of(ctx);

        // The disable funnel notifies OnRunDisabled; this catches any path
        // that somehow bypassed it (belt and braces — enabled off without a
        // callback still ends the run).
        if (!obs.disableFired && !rs.enabled)
        {
            obs.disableFired = true;
            std::lock_guard<std::mutex> lock(_obsMutex);
            if (_disableReason.empty())
                _disableReason = "run disabled (no reason captured)";
        }

        // The anchor the run is currently heading for, for the closing-distance
        // signal below.
        std::optional<DungeonBossInfo> const next =
            ctx->GetValue<std::optional<DungeonBossInfo>>(DcKey::NextDungeonBoss)->Get();

        uint32 mask = _lastMask;
        if (InstanceScript* inst = DcTargeting::GetInstanceScript(tank))
            mask = inst->GetCompletedEncounterMask();
        std::size_t const anchors =
            ctx->GetValue<std::unordered_set<uint32>&>(DcKey::ClearedAnchors)->Get().size();

        bool progressed = false;
        if (uint32 const fresh = mask & ~_lastMask)
        {
            for (uint32 bit = 0; bit < 32; ++bit)
            {
                if (!(fresh & (1u << bit)))
                    continue;
                DcTestRunRecord::BossKill kill;
                kill.t = _totalMs / 1000;
                kill.via = "mask";
                for (BossRef const& ref : _roster)
                    if (ref.encounterIndex == bit)
                    {
                        kill.entry = ref.entry;
                        kill.name = ref.name;
                        break;
                    }
                if (kill.name.empty())
                    kill.name = "encounter #" + std::to_string(bit);
                _record.bossTimeline.push_back(kill);
                progressed = true;
            }
            _lastMask = mask;
        }
        if (anchors > _lastAnchors)
        {
            for (std::size_t i = _lastAnchors; i < anchors; ++i)
            {
                DcTestRunRecord::BossKill kill;
                kill.t = _totalMs / 1000;
                kill.name = "objective";
                kill.via = "anchor";
                _record.bossTimeline.push_back(kill);
            }
            _lastAnchors = anchors;
            progressed = true;
        }
        if (progressed)
            _record.bossesKilled =
                std::min<uint32>(static_cast<uint32>(_record.bossTimeline.size()),
                                 _record.bossesTotal);

        // A kill is not the only evidence a run is alive. Keyed on kills alone
        // the watchdog fails runs that are working perfectly: a party can
        // legitimately spend more than NoProgressS on the trash between two
        // bosses — or on the way to the first one, where the clock starts at
        // zero and nothing has been killed yet.
        //
        // What counts is that something CHANGES, never that some state holds.
        // "In combat" held true is the signature of the phantom-combat
        // ghost-flag deadlock, so counting combat itself as progress would make
        // that bug undetectable by construction — the run would sit in a dead
        // fight forever and the watchdog would call it healthy the whole time.
        //
        // So compare this tick's combat picture against last tick's:
        //
        //   entering or leaving combat — a pack pulled, or a pack died.
        //   a different victim         — one mob down, on to the next.
        //   the victim's health moved  — the fight itself is progressing,
        //                                which is what carries a long single
        //                                target where nothing else changes.
        //
        // A real fight moves at least one of those every sample. A wedged one
        // moves none: stuck flag, no victim or a stale one, health frozen.
        bool const inCombat = tank->IsInCombat();
        Unit const* victim = tank->GetVictim();
        ObjectGuid const victimGuid = victim ? victim->GetGUID() : ObjectGuid::Empty;
        uint32 const victimHpPct =
            victim ? static_cast<uint32>(victim->GetHealthPct()) : 0u;

        if (inCombat != _lastInCombat || victimGuid != _lastVictim ||
            victimHpPct != _lastVictimHpPct)
            progressed = true;

        _lastInCombat = inCombat;
        _lastVictim = victimGuid;
        _lastVictimHpPct = victimHpPct;

        // Sampled every monitor step while somebody is still standing, so the
        // wipe verdict below can name what took the party down.
        TrackEngagement(tank);

        if (next.has_value() && next->mapId == tank->GetMapId())
        {
            // Re-arm on target change: distance to a NEW anchor is unrelated to
            // the best held for the old one, and would otherwise read as an
            // instant regression that never recovers.
            if (next->entry != _bestDistEntry)
            {
                _bestDistEntry = next->entry;
                _bestDist = -1.f;
            }
            float const dist = tank->GetDistance(next->x, next->y, next->z);
            if (_bestDist < 0.f || dist < _bestDist - DC_TESTRUN_PROGRESS_EPSILON_YD)
            {
                _bestDist = dist;
                progressed = true;
            }
        }

        if (progressed)
            _sinceProgressMs = 0;
        else
            _sinceProgressMs += dt;

        // Pause / stall trackers.
        if (rs.paused)
        {
            pauseReason = rs.pauseReason;
            if (!_wasPaused)
            {
                _record.pauses.push_back({_totalMs / 1000, pauseReason});
                _pausedForMs = 0;
            }
            else
                _pausedForMs += dt;
            _wasPaused = true;
        }
        else
        {
            _wasPaused = false;
            _pausedForMs = 0;
        }
        obs.paused = rs.paused;
        obs.pausedForMs = _pausedForMs;

        // Wipe tracker. The in-run death bailout (DungeonClearPartyDiedTrigger)
        // covers every case where SOMEONE is left alive to notice — it fires and
        // the disable funnel above ends the run with the death reason. It cannot
        // cover a full wipe: the trigger lives in the `dungeon clear` strategy,
        // which is on the non-combat and combat engines only, and a corpse runs
        // BOT_STATE_DEAD. So the harness has to see this one for itself.
        //
        // Dead = dead OR ghost (a released bot is still a wiped bot). Members off
        // the leader's map are ignored, matching the trigger's own convention:
        // someone alive back in town is not a party that is still fighting.
        bool anyAlive = tank->IsAlive();
        if (anyAlive)
            _lastAliveMember = tank->GetName();
        else if (Group* group = tank->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == tank || member->GetMapId() != tank->GetMapId())
                    continue;
                if (member->IsAlive())
                {
                    anyAlive = true;
                    _lastAliveMember = member->GetName();
                    break;
                }
            }
        }

        if (anyAlive)
            _wipedForMs = 0;
        else
            _wipedForMs += dt;
        obs.partyWiped = !anyAlive;
        obs.wipedForMs = _wipedForMs;

        stallReason = ctx->GetValue<std::string&>(DcKey::StallReason)->Get();
        if (!stallReason.empty())
            _stalledForMs += dt;
        else
            _stalledForMs = 0;
        obs.stalled = !stallReason.empty();
        obs.stalledForMs = _stalledForMs;

        obs.sinceProgressMs = _sinceProgressMs;
    }

    DcTestRun::Verdict const verdict = DcTestRun::Classify(obs, _limits);
    if (!DcTestRun::IsTerminal(verdict))
        return;

    std::string failReason;
    switch (verdict)
    {
        case DcTestRun::Verdict::Success:
            break;
        case DcTestRun::Verdict::FailDisabled:
        {
            std::lock_guard<std::mutex> lock(_obsMutex);
            failReason = "run disabled: " + _disableReason;
            break;
        }
        case DcTestRun::Verdict::FailPartyWiped:
        {
            std::string const who =
                _lastAliveMember.empty() ? std::string()
                                         : " (last standing: " + _lastAliveMember + ")";
            if (_engaged.Empty())
                failReason = "party wiped out of combat" + who;
            else if (_engaged.isBoss)
                failReason = "party wiped on " + _engaged.name + who;
            else
                failReason = "party wiped to trash: " + _engaged.name + who;
            break;
        }
        case DcTestRun::Verdict::FailPausedTimeout:
            failReason = "paused for over " + std::to_string(_limits.pauseGraceMs / 1000) +
                         "s: " + (pauseReason.empty() ? "(no reason)" : pauseReason);
            break;
        case DcTestRun::Verdict::FailStalledTimeout:
            failReason = "stalled for over " + std::to_string(_limits.stallGraceMs / 1000) +
                         "s: " + (stallReason.empty() ? "(no reason)" : stallReason);
            break;
        case DcTestRun::Verdict::FailNoProgress:
            failReason = "no boss/objective progress for " +
                         std::to_string(_limits.noProgressMs / 1000) + "s";
            break;
        case DcTestRun::Verdict::FailOverallTimeout:
            failReason = "exceeded the overall time limit (" +
                         std::to_string(_limits.overallTimeoutMs / 1000) + "s)";
            break;
        case DcTestRun::Verdict::FailAborted:
        {
            std::lock_guard<std::mutex> lock(_obsMutex);
            failReason = obs.leaderMissing ? "leader tank vanished"
                         : (_abortReason.empty() ? "aborted" : _abortReason);
            break;
        }
        default:
            break;
    }

    Finish(verdict, failReason);
}

void DcTestRunJob::FailSetup(std::string const& why)
{
    _record.setupStage = StageName(_stage.load());
    _record.result = "setup_failed";
    _record.failReason = why;
    LOG_INFO("playerbots.dungeonclear", "TESTRUN {} setup failed at {}: {}",
             _record.runId, _record.setupStage, why);
    Teardown();
}

void DcTestRunJob::Finish(DcTestRun::Verdict verdict, std::string const& failReason)
{
    _record.result = DcTestRun::VerdictName(verdict);
    _record.failReason = failReason;

    // Wipe post-mortem. Also filled for a death bailout — the run ends as
    // "disabled" with a corpse in the party, and "what killed us" is the same
    // question there. Left empty for every outcome nobody died in, so a
    // successful run that lost (and rezzed) a member carries no stale opponent.
    if (verdict == DcTestRun::Verdict::FailPartyWiped || AnyMemberDead(FindTank()))
    {
        _record.wipeOnBoss = _engaged.isBoss;
        _record.wipeOpponentEntry = _engaged.entry;
        _record.wipeOpponent = _engaged.name;
    }
    {
        std::lock_guard<std::mutex> lock(_obsMutex);
        _record.disableReason = _disableReason;
    }
    Teardown();
}

// Log the party out, whichever holder owns it.
//
// A bot's login is filed under the holder that owns its master *at callback
// time*, resolved by sWorldSessionMgr->FindSession(masterAccountId)
// (PlayerbotMgr.cpp AddPlayerBot / PlayerbotOperations.h OnBotLoginOperation).
// The test driver logs in through the masterless fake-session path, and those
// sessions are constructed directly — never handed to WorldSessionMgr::AddSession
// — so that lookup misses and every driver-run party lands in
// sRandomPlayerbotMgr instead of the driver's own PlayerbotMgr. The run itself
// never noticed (it resolves bots through ObjectAccessor), but LogoutPlayerBot
// keys off the holder's map and returned silently: the group disbanded, the
// bots left the instance, and then sat in the world logged in forever.
// A real GM's session *is* in WorldSessionMgr, so their runs took the first
// branch — which is why this only ever showed up on driver/dashboard runs.
void DcTestRunJob::LogoutBots(Player* gm)
{
    PlayerbotMgr* mgr = gm ? GET_PLAYERBOT_MGR(gm) : nullptr;
    for (Slot const& slot : _slots)
    {
        if (!slot.guid)
            continue;

        if (mgr && mgr->GetPlayerBot(slot.guid))
            mgr->LogoutPlayerBot(slot.guid);
        else if (sRandomPlayerbotMgr.GetPlayerBot(slot.guid))
            sRandomPlayerbotMgr.LogoutPlayerBot(slot.guid);
        else if (ObjectAccessor::FindConnectedPlayer(slot.guid))
            LOG_WARN("playerbots.dungeonclear",
                     "TESTRUN {} bot {} is online but owned by no holder — left logged in",
                     _record.runId, slot.guid.ToString());
    }
}

void DcTestRunJob::Teardown()
{
    // gates the observers off before DisableDungeonClear; the exchange also
    // makes a second Teardown (racing abort / double .dc test stop in one tick
    // gap) a no-op, so the record is appended and the bots logged out once.
    if (_stage.exchange(Stage::TearingDown) == Stage::TearingDown)
        return;

    Player* tank = FindTank();
    if (tank)
    {
        _record.finalMap = tank->GetMapId();
        _record.finalX = tank->GetPositionX();
        _record.finalY = tank->GetPositionY();
        _record.finalZ = tank->GetPositionZ();

        // Capture the full picture BEFORE anything below tears the party down:
        // DisableDungeonClear resets the run state, Group::Disband drops every
        // member, and LogoutBots removes them from the world. Afterwards there
        // is nothing left to describe why the run failed.
        _record.diag = DcDiag::Capture(tank, "teardown");
        _record.stallAtEnd = _record.diag.stallReason;
        _record.phaseAtEnd = _record.diag.phase;
        if (_record.diag.valid && _record.result != "success")
            LOG_INFO("playerbots.dungeonclear", "TESTRUN DIAG {} {}",
                     _record.runId, DcDiag::Summarize(_record.diag));

        if (PlayerbotAI* tankAI = GET_PLAYERBOT_AI(tank))
            if (DcRun::Of(tankAI).enabled)
                DcActionShared::DisableDungeonClear(tankAI, "test run teardown");
    }

    if (_tankGuid)
        DcSettings::ClearRun(_tankGuid);

    if (tank)
        if (Group* group = tank->GetGroup())
            group->Disband(true);

    Player* gm = FindGm();
    LogoutBots(gm);

    // Shed the permanent heroic saves the run just created so the pool bots go
    // back clean (and the instance can reset) — the mirror of the pre-teleport
    // unbind. Guid-keyed, so it works after the logout.
    if (_heroic)
        for (Slot const& slot : _slots)
            if (slot.guid)
                sInstanceSaveMgr->PlayerUnbindInstance(slot.guid, _mapId,
                                                       DUNGEON_DIFFICULTY_HEROIC,
                                                       /*deleteFromDB*/ true);

    _record.endedAtMs = NowUnixMs();
    _record.durationS = static_cast<uint32>((_record.endedAtMs - _record.startedAtMs) / 1000);
    DcTestRunRecord::Append(_record);

    LOG_INFO("playerbots.dungeonclear", "TESTRUN END {} result={} reason={} bosses={}/{} duration={}s",
             _record.runId, _record.result, _record.failReason,
             _record.bossesKilled, _record.bossesTotal, _record.durationS);

    if (gm)
        ChatHandler(gm->GetSession()).SendSysMessage(Acore::StringFormat(
            "Test run {}: {} ({}/{} bosses, {}s){}", _record.dungeon, _record.result,
            _record.bossesKilled, _record.bossesTotal, _record.durationS,
            _record.failReason.empty() ? "" : (" — " + _record.failReason)));

    _done = true;  // manager erases this job (and releases its reservations) next tick
}

void DcTestRunJob::OnRunDisabled(std::string const& reason)
{
    if (_stage.load() != Stage::Monitoring)
        return;

    std::lock_guard<std::mutex> lock(_obsMutex);
    if (_disableFired)
        return;  // first reason wins
    _disableFired = true;
    _disableReason = reason;
    _disableAllCleared = reason == DcActionShared::kReasonAllCleared;
}

void DcTestRunJob::OnStatusPayload(std::string const& payload)
{
    Stage const stage = _stage.load();
    if (stage != Stage::Monitoring && stage != Stage::Starting)
        return;

    // Payload: STATUS \t enabled \t bossEntry \t bossName \t stall \t skipped
    //          \t state \t detail \t pullSetting \t pullDecision
    std::vector<std::string> parts;
    std::size_t from = 0;
    while (from <= payload.size())
    {
        std::size_t const tab = payload.find('\t', from);
        if (tab == std::string::npos)
        {
            parts.push_back(payload.substr(from));
            break;
        }
        parts.push_back(payload.substr(from, tab - from));
        from = tab + 1;
    }
    if (parts.size() < 8 || parts[0] != "STATUS")
        return;

    std::lock_guard<std::mutex> lock(_obsMutex);
    if (parts[6] == _lastStatusState)
        return;
    _lastStatusState = parts[6];
    _record.statusTimeline.push_back({_totalMs / 1000, parts[6], parts[7]});
}
