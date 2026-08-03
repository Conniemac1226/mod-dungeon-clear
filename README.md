# mod-dungeon-clear

Autonomous dungeon-clearing mode for **mod-playerbots** tank bots, packaged as a
drop-in AzerothCore module. A tank bot drives the party from boss to boss,
clearing trash, pathing the layout, pausing for loot, resting between fights, and
handling doors and scripted events along the way. You deal damage and let the
tank run the dungeon.

Routes are generated on the fly from the live navigation mesh, with no waypoints
or hardcoded paths, so the clear works in any instance. It runs against a
**stock, unmodified mod-playerbots** checkout with no playerbots source edits.

> ## Use the companion addon
>
> [**mod-dungeon-clear-addon**](https://github.com/jrad7/mod-dungeon-clear-addon)
> is the recommended way to drive a clear: a movable in-game panel with On, Off,
> Skip, and Pause buttons, a live status readout, a per-boss list, and a settings
> panel for live tuning. The `dc` chat keywords and `.dc` commands still work, but
> the addon is easier to use.

## What it does

While enabled, the tank bot handles a run end to end:

- **Routing** from boss to boss over the live navmesh, including long corridors,
  doors, and multi-wing maps.
- **Pulling trash** on the way to each boss, with selectable pull styles (see
  [Pull modes](#pull-modes)).
- **Scripted events** that gate progress in many classic dungeons: levers,
  altars, gongs, freed prisoners, escorts, and off-mesh drops (see
  [Scripted dungeon events](#scripted-dungeon-events)).
- **Looting** finished corpses, with a quality floor and skip logic so the party
  does not camp on corpses with nothing worth taking.
- **Resting** between fights, tracking playerbots' own eat and drink thresholds.
- **Party support**: followers stay with the tank, healers reposition to keep the
  tank in line of sight, and the group regroups after a fight pulls it apart.
- **Death recovery**: when a member dies and a living Priest, Paladin, Shaman, or
  Druid remains, the run holds while they resurrect the fallen and resumes once
  everyone is back up, instead of ending on the first death (`DungeonClear.PostCombatRez`).
  The run still ends on a full wipe, with no living resurrector, or on timeout.

## Requirements

- **mod-playerbots** installed and enabled. This module is an extension of the
  playerbots AI engine. It subclasses playerbots' strategy, action, trigger, and
  value classes and links against them. It is not standalone.

## Install

1. Clone into `modules/mod-dungeon-clear/`.
2. Re-run CMake and rebuild the worldserver (`-DMODULES=static`).
3. Optionally copy `conf/mod_dungeon_clear.conf.dist` to
   `mod_dungeon_clear.conf`.

## Usage

Both input methods control the same behaviour and act on the group's **tank**
bot. Commands must come from a real player in the bot's group. `.dc on` requires
being inside a dungeon.

| Slash command | In-party chat keyword | What it does |
|---|---|---|
| `.dc on` | `dc on` / `dungeon clear on` | Start the clear. |
| `.dc off` | `dc off` / `dungeon clear off` | Stop and return bots to the player. |
| `.dc pause` | `dc pause` / `dungeon clear pause` | Soft-stop in place; resume with the same command. |
| `.dc skip` | `dc skip` | Skip the current objective if the tank is stalled. |
| `.dc pull` | `dc pull` / `dungeon clear pull` | Cycle the pull mode. |
| `.dc status` | `dc status` | Print the current run status. |
| `.dc bosses` | `dc bosses` | List the dungeon's bosses and kill state. |
| `.dc go <boss>` | | Route directly to a named boss. |
| `.dc spectate` | | Toggle the free-fly spectator camera. |

Non-tank party bots follow the tank only while it has dungeon clear enabled, then
revert to the player automatically.

## Pull modes

How the tank takes trash packs on the way to each boss. Choose from the addon's
pull control or the `dc pull` toggle.

| Mode | Behaviour | Speed | Risk |
|---|---|---|---|
| **Dynamic** *(recommended)* | Decide per pack: Leeroy a lone pack, camp-pull a clustered or oversized one. | Middle | Middle |
| **Leeroy** | Walk straight into each pack and fight in place. | Fastest | Highest |
| **Advanced** | Pull every pack back to a held camp before fighting. | Slowest | Lowest |

**Dynamic suits most content.** It Leeroys easy packs at full speed and only pays
the careful camp-pull cost on dangerous ones, estimating how many mobs would
actually aggro and comparing that to a tunable ceiling
(`DungeonClear.PullDynamicMaxLeeroyMobs`). Use **Leeroy** in content you can
out-gear, or **Advanced** in harder content where every pull matters.

See the [Pull modes](https://github.com/jrad7/mod-dungeon-clear/wiki/Pull-Modes)
wiki page for how each mode works in detail and how to tune Dynamic.

## Scripted dungeon events

Many classic instances gate progress behind a scripted step the party must
perform: pull a lever, click an altar, ring a gong, free a prisoner, escort an
NPC, or leap an off-mesh gap. mod-dungeon-clear performs these automatically as
part of the normal boss route, with no player input.

Event support currently covers Deadmines, Shadowfang Keep, Wailing Caverns,
Uldaman, Sunken Temple, Razorfen Downs, Scarlet Monastery, Zul'Farrak, Blackrock
Depths, Scholomance, Stratholme, and Dire Maul, and coverage continues to expand.
Faction-specific events run only for the relevant side. If an event cannot
complete (for example, a scripted NPC the party let die), the run either stalls
for the player or skips the step, depending on whether the step is required.

The full per-dungeon list is on the
[Scripted Dungeon Events](https://github.com/jrad7/mod-dungeon-clear/wiki/Scripted-Dungeon-Events)
wiki page.

## You cannot play *as* the tank with dungeon clear on

Dungeon clear drives the **tank bot's** AI, so the tank must be a bot. If you
personally control the tank, the AI and you fight over the same character. The
exception is **self-bot mode**: turn your own character into a self-bot
(`.playerbots bot self`) and let the AI drive. If you want hands on the keyboard,
play a follower instead.

## Configuration

All `DungeonClear.*` options live in `conf/mod_dungeon_clear.conf.dist`, and most
are overridable live, per run, from the companion addon's Settings panel. Common
ones: loot quality floor, rest health and mana targets, and the Dynamic pull
tuning. See the
[Configuration reference](https://github.com/jrad7/mod-dungeon-clear/wiki/Configuration)
wiki page.

A few mod-playerbots ranges also affect runs (`LootDistance`, `ReactDistance`,
`SightDistance`, `FollowDistance`). The rest gate tracks playerbots' own eat and
drink thresholds automatically, so no playerbots config change is required.

## How it integrates

mod-playerbots exposes no extension API, so this module appends its context
factories into the engine's shared registries on the first world tick, and
registers a `.dc` command plus a login hook for the `dungeon clear` strategy. It
touches **no** playerbots file. Details:
[How it integrates](https://github.com/jrad7/mod-dungeon-clear/wiki/How-It-Integrates).

## Automated test runs (`.dc test`)

A **GM-only** harness, not part of normal play. One command rolls a random
5-bot party (tank, healer, three DPS on five different classes), levels and
gears it, sends it to a dungeon entrance and lets it clear the place on its
own. You stay out of the party and watch, or `.dc spectate` along. Every run
logs its seed, so a run that hits a bug can be replayed with the exact same
party.

| Command | What it does |
|---|---|
| `.dc test start <dungeon> [level=N] [seed=N]` | Start a run. `<dungeon>` is a token from `list` (or a mapId). |
| `.dc test status` | Progress of each live run. |
| `.dc test stop [runId\|dungeon\|all]` | Abort runs. |
| `.dc test list` | Supported dungeons and their default levels. |
| `.dc test watch [runId\|dungeon]` | Ride a live run's camera. Hides you, sends you to that run's instance and follows its tank. Run it again for another run to hop straight there — the previous instance is left and unbound for you. `off` ends it and sends you back where you started. |
| `.dc test watch next` | Hop to the next live run, wrapping — repeat it to tour every run without looking up ids. Stays put when there is nowhere to go (no live run, or the only one is already up). |
| `.dc test plan start <dungeon> total=N [concurrent=N]` | Run the same dungeon N times, up to `concurrent` at once, then write a summary. |
| `.dc test plan status` | Progress of each live plan. |
| `.dc test plan stop [planId\|all]` | Stop a plan and its runs. |

These also work from the **worldserver console** and the AC Command Deck. For
console use, create a dedicated bot account with one character named per
`DungeonClear.TestRun.DriverCharacter` (default `Dcdriver`); the module logs it
in and parks it as the stand-in GM.

A run succeeds when every boss is down. It fails on a party wipe, a long stall,
no progress, or the overall time cap (tunable via the `DungeonClear.TestRun.*`
settings). Results land in `dc_testruns.jsonl` next to the worldserver, with
the run's log lines bracketed by `TESTRUN START/END` markers. Wing-split
dungeons run one wing at a time (`dm-east`, `sm-cath`, ...).

## License

AGPL-3.0-or-later (inherited from mod-playerbots). See `LICENSE`.
