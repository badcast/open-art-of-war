# openaow-server — Dedicated Server

`openaow-server` is the standalone headless **dedicated server** for **Open Art Of War**. It is written in C++17 and links only `OpenAOWNetworking` (which embeds ENet 1.3.18 and uses `libarchive` for `.awr` replay archives) plus `nlohmann_json`—with zero SDL, OpenGL, audio, or gameplay engine dependencies.

## Purpose & Architecture

`openaow-server` runs no game simulation and requires no game assets. All connections are initiated outbound by players over UDP, so players behind NAT routers do not need forwarded ports.

The server performs three roles:

1. **Public Room Directory & Matchmaking (`RoomServer`)**
   - Listens on the master UDP port (`--port`, default `38100`) for `ListRooms`, `CreateRoom`, and `FindRoom` requests, and sends `ServerInfo` (`--name`, `--motd`) when a client connects.
   - Allocates dedicated rooms on demand from the `--room-ports` UDP range (`38110`–`38199`), each backed by its own `SessionHost` instance and identified by a 6-character code (alphabet `ABCDEFGHJKLMNPQRSTUVWXYZ23456789`, omitting `0/O` and `1/I`).
   - Inside a dedicated room, the room owner (the lowest-numbered connected slot) chooses the map (`OwnerSetMap`), uploads map data (`FileData`), configures AI players, teams, seats, and match options (`OwnerSetAi`, `OwnerSetTeam`, `OwnerSetSeat`, `OwnerSetOptions`), and starts the match (`OwnerStart`).

2. **NAT Hole-Punching Rendezvous (`RegisterHost` / `JoinHosted`)**
   - Preserves peer-to-peer hosting on desktop and mobile (`NetMatch::host` + `NetMatch::Announce`) without requiring router port forwarding on the host.
   - `HostAnnouncer` registers the player-hosted game on the master port and sends raw UDP `Hello` datagrams (`RawKind::Hello` with a 16-byte `SocketToken`) from its game socket so `RoomServer` learns the socket's public address and port.
   - When a joining client sends `JoinHosted`, `RoomServer` exchanges endpoints (`PunchRequest` to the host, `HostedEndpoint` with public and LAN addresses to the client) so both peers punch UDP mappings to each other—or connect directly over LAN when both share the same public IP.

3. **Game Room Packet Relay (`SessionHost`)**
   - **Turn relay**: Collects player `Commands` on `Channel::Lockstep`, advances turns on its clock (`--turn-ms`, adaptive input delay clamped to `--input-delay-min`..`--input-delay-max`), and broadcasts `TurnBundle` messages to all peers in the room.
   - **Authority/Replica relay**: When a match starts in Authority mode (`MatchSetup::authorityMode`), the room owner at match start becomes `m_authoritySlot`. `SessionHost` validates the sender slot and `matchId`, relays per-recipient `AuthorityState` world snapshots from the authority to the target replica on `Channel::Bulk`, and relays `AuthorityAck` messages from replicas back to the authority on `Channel::Control`. Replicated state is transmitted as fog-filtered, ack-tracked delta **world snapshots** (`WorldSnapshot`: `1/16 px` sub-tile quantisation, varint packing, and **Zstandard (`zstd`)** frame compression with a reused `thread_local` context at `ZSTD_CLEVEL_DEFAULT` plus frame checksums and bounded decoding, carried over ENet's range-coder and CRC-32 transport). `openaow-server` forwards these already-compressed `AuthorityState` payloads untouched—without decompressing or re-compressing them—so snapshot compression adds zero CPU cost on the dedicated server.
   - **Lockstep fallback**: When Authority mode is off, every peer simulates locally from `TurnBundle` streams and sends `WorldHash` checksums on `Channel::Lockstep`; `SessionHost` compares reported hashes and broadcasts `Desync` if any differ.

## Building & Running

`openaow-server` is enabled by default via the CMake option `OAOW_BUILD_SERVER=ON`.

From the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target openaow-server --parallel
```

This produces `./build/openaow-server` (or `build\openaow-server.exe` on Windows).

Run with default settings or with a JSON configuration file:

```bash
./build/openaow-server
./build/openaow-server --name "My Server" --motd "Welcome!"
./build/openaow-server --config server.json
```

Send `SIGINT` or `SIGTERM` for a clean shutdown (any in-progress room replays are flushed before exit).

## CLI & `--config` Reference

Options are resolved in order: built-in defaults → `--config <file.json>` → command-line flags (CLI flags always override `--config`).

- Value flags accept both `--key=value` and `--key value`.
- `--config <file.json>` reads a JSON object whose keys match the CLI flag names without leading dashes (e.g. `"port": 38100`, `"room-ports": "38110-38199"`). JSON comments are permitted.
- Unknown flags or unknown JSON keys cause `openaow-server` to exit with status `2`.

| Option / JSON Key | Default | Description |
| --- | --- | --- |
| `--config <file.json>` | *(none)* | CLI-only: load options from a JSON file (keys without `--`). |
| `--bind <address>` | `""` (all interfaces) | IP address to bind for both UDP sockets and the TCP status endpoint. |
| `--port <n>` | `38100` | Master UDP port for directory queries, room creation/lookup, and P2P rendezvous. |
| `--room-ports <first-last>` | `38110-38199` | Inclusive UDP port range allocated to dedicated rooms (`1`–`65535`, `first <= last`). |
| `--tcp-port <n>` | `38100` | TCP port for the HTTP JSON status endpoint (`0` disables it). |
| `--max-rooms <n>` | `64` | Maximum concurrent dedicated rooms (also caps player-hosted listings at `4 × max-rooms`). |
| `--max-players <n>` | `8` | Maximum players per room, clamped to `2..8`. |
| `--max-clients <n>` | `512` | Maximum concurrent ENet connections on the master UDP port. |
| `--turn-ms <n>` | `100` | Turn duration in milliseconds, clamped to `20..500`. |
| `--input-delay-min <n>` | `1` | Minimum turn input delay, clamped to `1..20`. |
| `--input-delay-max <n>` | `6` | Maximum adaptive turn input delay, clamped to `input-delay-min..30`. |
| `--idle-room-timeout-s <n>` | `120` | Seconds before an active match room with no connected players closes (reconnect window). |
| `--empty-room-timeout-s <n>` | `30` | Seconds before a lobby closes after the last player leaves. |
| `--unused-room-timeout-s <n>` | `120` | Seconds before a newly created room closes if nobody ever joins it. |
| `--dir-replay <dir>` | `"replay"` | Directory for `.awr` match replays (`""` disables recording). **See privacy note below.** |
| `--name <text>` | `"OpenAOW server"` | Server name shown to clients and in the status JSON. |
| `--motd <text>` | `""` | Message of the day sent to clients on master connect. |
| `--log-level <level>` | `"info"` | Minimum log level: `trace`, `debug`, `info`, `warn`, or `error`. |
| `--log-file <file>` | *(none)* | Append log lines to this file in addition to stdout/stderr. |
| `--stats-interval-s <n>` | `60` | Seconds between summary log lines (`rooms`, `matches`, `players`, `browsing`); `0` disables. |
| `--version` | — | CLI-only: print `openaow-server <version> (protocol <n>)` and exit `0`. |
| `--help` (`-h`) | — | CLI-only: print help text and exit `0`. |

Example `server.json`:

```json
{
  "name": "My OpenAOW Server",
  "motd": "Dedicated rooms and rendezvous",
  "port": 38100,
  "room-ports": "38110-38199",
  "tcp-port": 38100,
  "max-rooms": 64,
  "max-players": 8,
  "dir-replay": "/var/lib/openaow-server/replay",
  "log-file": "/var/log/openaow-server.log",
  "stats-interval-s": 60
}
```

## HTTP JSON Status Endpoint (`--tcp-port`)

When `--tcp-port` is greater than `0` (default `38100`), `StatusPage` opens a non-blocking TCP listener on that port (and `--bind` if set). Every incoming HTTP request receives an `HTTP/1.0 200 OK` response with `Content-Type: application/json`, `Access-Control-Allow-Origin: *`, and a JSON payload:

```json
{
  "name": "OpenAOW server",
  "rooms": 1,
  "players": 2,
  "matches": 1,
  "master_clients": 3,
  "list": [
    {
      "code": "ABC234",
      "name": "Skirmish",
      "version": "1.0.0-alpha",
      "map": "assets/maps/skirmish_1.json",
      "players": 2,
      "running": true,
      "port": 38110,
      "listed": true
    }
  ]
}
```

Because `StatusPage` serves plain HTTP without authentication, either keep `--tcp-port` firewalled / bound to `127.0.0.1` behind a reverse proxy or pass `--tcp-port 0` if you do not want public HTTP monitoring.

## Server-Side Replay Recording (`--dir-replay`)

When `--dir-replay` is non-empty (default `replay/` in the working directory), each room saves an `.awr` archive (`open-art-of-war-YYYYMMDD-HHMMSS_<map>.awr`) when a match ends (all players leave, the authority leaves, or the room closes) if at least one turn was closed.

- Server replays record `replay.json`, `setup.bin`, `turns.bin`, and (in Lockstep mode) `checksums.bin`. They do not bundle `maps/` or `data/` files, so playback requires the matching map on the client.
- In Authority/Replica matches, replicas do not send `WorldHash` messages, so `checksums.bin` is empty.

> [!WARNING]
> **Privacy:** Server-generated `replay.json` files record every player's connection `addresses[]` (`IP:port`), and server logs record connection, join, and disconnect addresses (`--log-file`). Keep the `--dir-replay` directory and log files private (for example, `StateDirectoryMode=0750` in systemd), or disable server replay recording with `--dir-replay ""`.

## Seamless Updates & Multi-Version Coexistence

- **Multi-version coexistence without server restarts (`kProtocolVersion == 1`):** `openaow-server` keeps `kProtocolVersion` at `1` (`src/networking/include/openaow/net/Protocol.hpp`) and runs no game simulation. Each dedicated room (`CreateRoom`) and player-hosted listing (`RegisterHost`) records its creator's `gameVersion`, and `RoomServer::listing` filters `ListRooms` results to matching `gameVersion` strings (`RoomServer.cpp`). Inside each room, `SessionHost::handleHello` checks `gameVersion`, `simBuildId`, and `assetsHash` per room—and for mismatched content when `Hello::canSync` is enabled on a host with `dataManifest`/`dataFile`, serves `DataManifest` and `FilePathData` so the client loads a temporary session overlay. Rolling client or content updates therefore do not disrupt running matches or require restarting `openaow-server` unless `kProtocolVersion` itself changes.
- **Seamless Authority/Replica recovery:** During an Authority/Replica match, a replica that disconnects and reconnects (using its `SessionToken`) or misses a delta snapshot base recovers without restarting the match:
  - On disconnect and on `Hello` resume (`resumed`), `SessionHost` resets that slot's `authorityAcked` / `authorityAckTick` state, broadcasts `Roster` updates (causing the authority's `NetMatch::PublishSnapshot` to reset `SnapshotPeer` and send a full snapshot), and resends `StartMatch`, `m_turnLog` (`TurnBundle` catch-up), and active `PauseState` if the match is paused.
  - If a replica receives a delta `AuthorityState` whose `baseTick` is missing or fails to expand, it sends an `AuthorityAck` NACK (`tick == 0`), prompting the authority to reset that peer's delta baseline and send a fresh full snapshot instead of stalling.

## Firewall & VPS Deployment Notes

- **Ports to open on the server:**
  - `UDP 38100` (`--port`): master server (directory, room codes, NAT rendezvous).
  - `UDP 38110-38199` (`--room-ports`): dedicated room instances. Ensure the port range has at least as many ports as `--max-rooms`.
  - `TCP 38100` (`--tcp-port`, optional): HTTP JSON status page.
- **Lightweight relay design:** Because `openaow-server` runs no simulation, AI, graphics, or asset loading, CPU and memory overhead stay minimal; its main loop polls sockets and sleeps `1 ms` per iteration.
- **systemd service:** A reference unit file is provided at [`openaow-server.service`](openaow-server.service). It runs `openaow-server` as an unprivileged `openaow` user with `ProtectSystem=strict`, `NoNewPrivileges=true`, and a private `StateDirectory=openaow-server` (`0750`) for `--dir-replay /var/lib/openaow-server/replay`.
