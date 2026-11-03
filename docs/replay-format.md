# Replay files (`.awr`, Art of War Replay)

A replay is a **tar archive compressed with zstd** (`tar --zstd -xf file.awr` opens it). It holds a whole network match:
the setup, every turn the host closed, the world checksums and (in a player's own file) the map and the simulation data.
The simulation is deterministic, so feeding the turns back reproduces the match exactly; the checksums prove it.

Name: `open-art-of-war-YYYYMMDD-HHMMSS_<map>.awr` (local time of the match start).

| Where | Written by | Folder |
| --- | --- | --- |
| Player | every client at the end of a network match (victory, defeat, leaving, closing the game) | the user's data directory, `replays/` (never next to the game or in `assets/`) |
| Server | `openaow-server`, every match of every room | `replay/` next to the server, or `--dir-replay <dir>` (empty = off) |

The server also logs the address of every connection (`connection from`, `player joined/back/disconnected`) and keeps the
addresses of each player in its replay's `replay.json`. Both are private data of the administrator.

## Entries

| Entry | Content |
| --- | --- |
| `replay.json` | Meta data, JSON: `format` (1), `game_version`, `protocol`, `source` (`client`/`server`), `recorded_at`, `map`, `map_hash`, `assets_hash`, `seed`, `turn_ms`, `ticks_per_turn`, `turns`, `ticks`, `players[]`, `starting_credits`, `fog`, `prebuilt`, `night` (time of day; missing = day), `local_slot`, `result {outcome, tick}` (client) or `end_reason`, `desynced` (server), `checksum_interval`, `mode` (`network`, `custom`: a custom game of this computer, `campaign`; missing = `network`), for a campaign mission also `mission`, `player_faction`, `difficulty` (the setup then only names the map and the two sides: playback loads the mission). A server file adds `addresses[]` to each player |
| `setup.bin` | The match setup bytes (`NetMatch::EncodeSetup`): map, seed, slots. The truth about the match; the names in the meta are only labels |
| `turns.bin` | Repeated `u32 length (LE)` + an encoded `TurnBundle` message (`Net::encodeTurn`), turn 0, 1, 2, ... |
| `checksums.bin` | Repeated `u32 tick` + `u64 world checksum` (LE), every 20 ticks |
| `maps/<name>.json` | Client file: the map the match was played on |
| `data/assets/<path>` | Client file: the simulation data files (`net_hash` of `main.json`, plus the manifest). Playing the file puts them into a session overlay, so a replay keeps playing the way it was recorded after the game's data change |

A server file has no `maps/` and `data/` entries (the server does not know the game's files): the map with the same
`map_hash` must be on the computer that plays it.

## Camera

Each client sends `CommandType::View` (camera centre in world pixels, zoom x100) about once a second while it moves.
It travels with the turns, does nothing in the simulation, and lets the player follow a player's view.

## Safety of reading

A replay can come from anyone. `Net::readReplay` accepts only regular files named as above, caps an entry at 32 MB, the whole
file at 96 MB, 160 entries and 2 million turns, refuses names with `..`, absolute paths or other characters than
`[A-Za-z0-9._-/]`, and checks that the lists are well-formed. Data files are parsed (JSON/YAML) before they are used.

## Playing

Main menu -> OPEN REPLAY (list of `replays/`), a file dropped on the window, or `openaow --replay=<file.awr>`.
Space pause, `+`/`-` speed (x0.5 ... x8), the view combo box (or Tab: observer / each player), F follow the camera of the watched player,
the "Voice the interface" check box above the controls (off by default: UI sounds, announcer, unit replies and alerts are muted),
Home restart, Esc leave. The world can only go forward: going back restarts from the first turn.

A match of this computer alone (a custom game, a campaign mission) is recorded by the client too: one turn per tick
(`ticks_per_turn` 1), each with the commands the player issued on it, the checksums every `checksum_interval` ticks and
the player's camera as `View` commands, like a network match's.
