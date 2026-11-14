# Replay files (`.awr`, Art of War Replay)

A replay is a **tar archive compressed with zstd** (`tar --zstd -xf file.awr` opens it). It holds a whole match:
the setup, every turn the host closed, optional world checksums, and (in a player's own file) the map and the simulation data.
Because the simulation is deterministic, feeding the recorded turns back in **Standalone** mode reproduces the match;
when checksums were recorded by the same build, they verify that playback stayed identical.

Name: `open-art-of-war-YYYYMMDD-HHMMSS_<map>.awr` (local time of the match start).

| Where | Written by | Folder |
| --- | --- | --- |
| Player | every client at the end of a network match (victory, defeat, leaving, closing the game) | the user's data directory, `replays/` (never next to the game or in `assets/`) |
| Server | `openaow-server`, every match of every room | `replay/` next to the server, or `--dir-replay <dir>` (`""` = off) |

The server also logs the address of every connection (`connection from`, `player joined/back/disconnected`) and keeps the
addresses of each player in its replay's `replay.json`. Both are private data of the administrator.

## Entries

| Entry | Content |
| --- | --- |
| `replay.json` | Meta data, JSON: `format` (1), `game_version`, `protocol`, `sim_build_id`, `source` (`client`/`server`), `recorded_at`, `ended_at` (server), `map`, `map_hash`, `assets_hash`, `seed`, `turn_ms`, `ticks_per_turn`, `turns`, `ticks`, `players[]`, `starting_credits`, `fog`, `prebuilt`, `night` (time of day; missing = day), `local_slot`, `result {outcome, tick}` (client) or `end_reason`, `desynced`, `desync_tick` (server), `checksum_interval`, `mods[]` (client when active without session overlay), `mode` (`network`, `custom`: a custom game of this computer, `campaign`; missing = `network`), for a campaign mission also `mission`, `player_faction`, `difficulty` (the setup then only names the map and the two sides: playback loads the mission). A server file adds `addresses[]` to each player |
| `setup.bin` | The match setup bytes (`NetMatch::EncodeSetup`): map, seed, slots, options, tech levels, and `authorityMode`. The truth about the match; the names in the meta are only labels |
| `turns.bin` | Repeated `u32 length (LE)` + an encoded `TurnBundle` message (`Net::encodeTurn`), turn 0, 1, 2, ... |
| `checksums.bin` | Repeated `u32 tick` + `u64 world checksum` (LE), every 20 ticks (may be empty for replica-recorded or server-recorded Authority/Replica matches) |
| `maps/<name>.json` | Client file: the map the match was played on |
| `data/assets/<path>` | Client file: the simulation data files (`net_hash` of `main.yaml`, plus the manifest). Playing the file puts them into a session overlay, so a replay keeps playing the way it was recorded after the game's data change |

A server file has no `maps/` and `data/` entries (the server does not know the game's files): the map with the same
`map_hash` must be on the computer that plays it.

## Authority/Replica mode

In Authority/Replica network matches (`authorityMode` enabled in `setup.bin`), the room owner's client runs the
simulation (`Simulation::ExecutionMode::Authority`) and streams per-recipient `AuthorityState` snapshots to the other
players (`Simulation::ExecutionMode::Replica`). `.awr` replays **never store snapshots**; instead, `SessionHost` still
closes `TurnBundle` messages for all players' commands (including `CommandType::View` camera updates and `PlayerLeft`
departures), so `.awr` recording remains compact and command-based:

| Recorder | Behavior in Authority/Replica mode | `checksums.bin` |
| --- | --- | --- |
| **Authority client** | `NetMatch::CanRunTick` records every closed `TurnBundle` (`ReplayRecorder::OnBundle`) and runs the simulation; `NetMatch::AfterTick` records local world checksums (`OnChecksum`) every `checksum_interval` ticks without sending `WorldHash` over the network. | Populated (local authority checksums) |
| **Replica client** | Replicas do not run `SimulationStep`, so `NetMatch::PumpReplica` drains each incoming `TurnBundle` from `SessionClient`, records it via `ReplayRecorder::OnBundle`, and drops consumed turns. `maps/` and `data/` are bundled as usual when `FinishReplay` writes the file. | Empty (replicas do not simulate) |
| **Dedicated server (`--dir-replay`)** | `SessionHost` records `turns.bin` from every `TurnBundle` it closes and writes the `.awr` file when the match ends or the room closes. | Empty (clients do not send `WorldHash` in Authority mode) |

During playback, `Application::OpenReplay` / `ReplayPlayer` always restores **Standalone** deterministic simulation
(`Simulation::ExecutionMode::Standalone`) and feeds `turns.bin` through `NetMatch::ScheduleBundle`. Because all player
commands and `PlayerLeft` events are present in `turns.bin`, replays recorded by the authority, a replica, or
`openaow-server` all play back identically—including switching between Observer view and any player's fog-of-war view—and
skip checksum verification gracefully when `checksums.bin` is empty.

## Camera

Each client sends `CommandType::View` (camera centre in world pixels, zoom x100) about once a second while it moves.
It travels with the turns, does nothing in the simulation, and lets the viewer follow a player's view.

## Safety of reading

A replay can come from anyone. `Net::readReplay` accepts only regular files named as above, caps an entry at 32 MB, the whole
file at 96 MB, 160 entries and 2 million turns, refuses names with `..`, absolute paths or other characters than
`[A-Za-z0-9._-/]`, and checks that the lists are well-formed. Data files are parsed (JSON/YAML) before they are used.

## Playing

Main menu -> OPEN REPLAY (list of `replays/`), a file dropped on the window, or `openaow --replay=<file.awr>`.
Space pause, `+`/`-` speed (`x0.5` ... `x32`), the view combo box (or Tab: observer / each player), F follow the camera of the watched player,
the "Voice the interface" check box above the controls (off by default: UI sounds, announcer, unit replies and alerts are muted),
Home restart, Esc leave. The world can only go forward: going back restarts from the first turn.

A match of this computer alone (a custom game, a campaign mission) is recorded by the client too: one turn per tick
(`ticks_per_turn` 1), each with the commands the player issued on it, the checksums every `checksum_interval` ticks and
the player's camera as `View` commands, like a network match's.
