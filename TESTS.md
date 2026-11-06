# Tests and debugging

Everything there is to test, trace and inspect the game, the network code and the editors.

## 1. Two kinds of builds

| Build | What it has |
| --- | --- |
| Release (`build/`) | The game only. `--debug`, `--test`, the ImGui debugger, cheats and every `OAOW_*` developer variable are compiled out (a player's environment cannot change the game). `ctest` still works. |
| Debug variant (`-DAOW_DEBUG=ON`) | Everything below. Build it in its **own directory**, never reconfigure `build/`. |

```bash
# release + unit tests
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j16

# debug variant (optimised, but with the debug features)
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Release -DAOW_DEBUG=ON
cmake --build build-debug -j16
```

Run the game from the repository root (it looks for `./assets`). Without a screen, use
`SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`.

CMake options: `OAOW_BUILD_NET_TESTS` (default ON, network unit tests), `OAOW_BUILD_SERVER` (default ON).

## 2. Unit tests (`ctest`)

```bash
(cd build && ctest --output-on-failure)
```

| Test | Source | Covers |
| --- | --- | --- |
| `openaow-net-tests` | `src/networking/tests/NetTests.cpp` | CRC32, bit stream, protocol codec round trips, fuzzing the decoder, ENet over localhost, the lockstep session (turn order, late commands, desync detection, reconnect with the token, takeover of a slot the host still holds), lobby rules, password and leaving, ENet session, LAN discovery, room server, player-hosted games through the server. Prints `N checks, 0 failed`. |
| `openaow-charactereditor-tests` | `openaow-charactereditor/tests/ConfigMergeTests.cpp` | Saving YAML configs keeps what the editor does not know: unknown fields, comments, removed keys stay removed. |

Run one binary directly to see its whole output: `./build/openaow-net-tests`.

Not covered: packet loss, latency or reordering (the loopback transport has no loss model); the game side of a
network match (`NetMatch`) and the world checksum. Use the bot matches below for those.

## 3. Headless game tests (`--test`, debug build)

```bash
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./build-debug/openaow --test
```

The run ends with `Headless test result: SUCCESS` (exit code 0) or `FAILURE`. Aliases: `--headless`,
`--headless-test`, `-t`. Source: `src/test/HeadlessTest.cpp`; `HeadlessTest::run()` lists every check in order.

What the full run checks (in this order):

- Localization (gettext, en ↔ ru), YAML character configs, settings and the user directory (round trip,
  clamping, zoom / quality options).
- Campaign missions (8 Resistance missions with base positions, defense waves, Confederation campaign).
- Base construction, production prerequisites and queues (cancel refunds), destructible walls, AI rebuilding
  bases and walls.
- Test map (`assets/maps/testmap.json`): unit blocking, closed passages, wall breaching.
- Build radius (HQ 25 tiles, lab +5), Stormcast, siege units (Rhino transform, Grad range).
- Raycast bullets, retreat orders, recoil, camera bounds; wall targeting, rubble, flamethrower, crouching attack.
- Bunker construction and autonomous combat; fog of war (hidden → visible → explored, AI blind in fog);
  AI difficulty (`assets/characters/difficulty.yaml`).
- Damage to moving units, muzzle points, projectile spread, splash, burning deaths, multi-weapon units.
- Vehicle rotation, chain aggro, command override, choke points and anti-stacking, building damage, unit
  remains, cohesive group movement, sidestepping, wall factions.
- Construction rules, demolition, siege research, radar alarm, voices of each language, the AI keeping its base
  exit open, enemy structures under fog, prebuilt bases vs. start from scratch.
- Determinism: a skirmish played twice from recorded commands must give the same checksums
  (`RunDeterminismTest`), a full match and an AI-vs-AI match likewise (`RunMatchTest`, `RunMatchAiTest`).

### Environment switches of the headless tests

| Variable | Effect |
| --- | --- |
| `OAOW_TEST_ONLY=group` | Only the group movement test (`RunGroupMovementTest`) |
| `OAOW_TEST_ONLY=ai` | Only the AI behaviour test (`RunAiBehaviourTest`) |
| `OAOW_TEST_ONLY=combat` | Only the combat behaviour test (`RunCombatBehaviourTest`) |
| `OAOW_TEST_ONLY=makeway` | Only the make-way test (`RunMakeWayTest`) |
| `OAOW_TEST_VERBOSE=1` | Extra output: stuck units, squad states over time, distances in the manual attack test |

## 4. Debug mode in the game (`--debug`, debug build)

```bash
./build-debug/openaow --debug          # or -d
./build-debug/openaow --debug --testmap   # straight onto the test polygon (also --test-map, -tm)
./build-debug/openaow --debug --map=m26 --from-scratch
```

- Debug toolbar at the bottom of the screen: passability grid on/off, ImGui debugger on/off.
- Full control over the enemy units.
- **ImGui debugger** (`src/debug/DebugOverlay.cpp`):
  - *Economy & Cheats*: +10,000 credits, spawn a player / enemy tank, disable fog of war.
  - *AI Director*: override difficulty, show groups and leash lines, show chokepoints, AI ignores fog, wave size
    multiplier, AI credits (set, +500, +2000, bankrupt), research state.
  - *AI Control & Thoughts*: the computer player's log (auto-scroll, clear).
  - *Mission Controller*: current mission, objective, kills (the M0–M7 buttons are a stub and do nothing yet).
  - *Sound & Audio Tracks*: current track, menu theme, random track, mute.
  - *Entity Inspector*: every entity and its state.
- In debug mode the world rendering also draws debug information. Holding Alt (in any mode) shows the health
  bars of every unit.

In a network game the debug powers are switched off (commands from the network never carry the debug flag).

## 5. Network games and bot matches

### Command line

```bash
./build-debug/openaow --host[=port] --players=N --map=<name> [--name=<player>]   # LAN host (port 38100)
./build-debug/openaow --join=<address>[:port] [--name=<player>]                   # LAN client
./build-debug/openaow --server=<host>[:port] --server-create                      # room on a dedicated server
./build-debug/openaow --server=<host>[:port] --server-code=<code>                 # join a room by code
```

The host starts the match as soon as `--players` players are in.

### Developer variables (debug build only)

| Variable | Effect |
| --- | --- |
| `OAOW_NET_BOT=1` | The local player plays itself: sends all of its units at the nearest enemy. Only attacks: it does not build, research or place walls. |
| `OAOW_NET_TRACE=1` | Prints checksums on stdout: every hash tick (every 20 ticks) `[Net] tick N parts entities … credits … walls … research …` (running hash after each part), and every 200 ticks `[Net] tick N checksum …`. |
| `OAOW_NET_DROP_AT=<tick>` | The connection drops once at that tick; the reconnect logic takes over. |
| `OAOW_NET_FACTION=0\|1` | Everybody plays this faction. |
| `OAOW_NET_FACTION_BY_SLOT=1` | Even slots Resistance, odd slots Confederation. |

### Bot against bot (desync hunting)

```bash
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy OAOW_NET_BOT=1 OAOW_NET_TRACE=1
timeout 600 ./build-debug/openaow --host=38100 --players=2 --map=m26 --name=Bot1 > host.log 2>&1 &
sleep 2
timeout 600 ./build-debug/openaow --join=127.0.0.1:38100 --name=Bot2 > client.log 2>&1
wait
diff <(grep parts host.log) <(grep parts client.log)   # only the last line may differ (one side stopped first)
grep -E "DESYNC|Connection lost|Reconnected|Rejected" host.log client.log
```

Drop the `SDL_*` variables to watch both games in windows.

### Reading a desync

- The game shows `OUT OF SYNC at tick N` and the log `[Net] DESYNC: the worlds of the players differ`. The host
  compares the hashes of every 20th tick once all connected players have reported it.
- With `OAOW_NET_TRACE=1` on **both** peers, find the first tick whose `parts` line differs and the first part
  that differs there: `entities` (positions, hp, targets, states), `credits`, `walls`, `research`. Each value is
  a running hash, so everything after the first differing part differs too.
- The checksum does not cover RNG state, production queues, paths, projectiles, squads/AI or fog sight:
  a divergence there shows up only once it reaches a hashed field, i.e. later than it started.
- Network statistics in the corner (ping, jitter, loss, traffic, input delay, buffered turns, DESYNC):
  Settings → Video → *Network statistics*. Tab in a match shows the scoreboard.

### Bad network on one machine

```bash
sudo tc qdisc add dev lo root netem delay 600ms 300ms loss 3%   # all localhost traffic
sudo tc qdisc show dev lo
sudo tc qdisc del dev lo root                                    # remove it afterwards
```

## 6. Sound

| Variable | Effect |
| --- | --- |
| `OAOW_SFX_TRACE=1` | Logs every sound that starts (`[Sfx] <event> gain … pan …`) and every announcer line, to check events without listening. |

At start the log reports what was loaded: `[SfxMixer] N sound clips for M events`, `[VoiceManager] N voice lines in '<lang>'`.

## 7. Game data guarantee

- `assets/main.yaml` names every file the game loads by its full path; the game does not start without it.
  `tools/pack_assets.py` refuses to pack when main.yaml names a file that does not exist.
- `tools/pack_assets.py <assets> <out.blob>` packs the data and writes the integrity certificate into the blob
  itself: the entry `certificate.sha256` (`<path>  <sha256>` per packed file). There is no separate certificate
  file. With `-DBUILD_ASSETS_BLOB=ON` CMake runs it.
- When the game loads a blob it checks it against that certificate: no certificate, a missing or changed file, or a
  file the certificate does not list stops the game with the list of problems
  (`[Assets] N files checked against <blob>:certificate.sha256: all match` in the log). A plain `assets/` directory
  (development) carries no certificate and is not checked.
- Try it: pack a blob into an empty folder, start the game there (no `assets/` next to it, so the blob is used);
  then change a file inside the blob without repacking and start again.

## 8. Map editor

```bash
./build-debug/openaow-mapeditor --test                       # all editor self-tests
./build-debug/openaow-mapeditor --test-generator             # only the map generator (10 maps: reproducible, seamless, bases connected)
./build-debug/openaow-mapeditor --test-collision             # only the tile collision table
./build-debug/openaow-mapeditor --generate-map out.json [seed] [tileset]   # write a generated map and exit
```

## 9. Other checks

- **Translations**: every project has its own catalogs under `po/`:
  `po/openaow_{en,ru}.po`, `po/openaow_character_editor_{en,ru}.po`, `po/openaow_map_editor_{en,ru}.po`.
  Check with `tools/localegen.sh --check` or `msgfmt -c po/<file>.po`. CMake compiles them into
  `assets/locale/*/LC_MESSAGES/*.mo`.
- **Network assets check**: everything under `assets/data/*.json` and `assets/characters/*.yaml` (except the
  sound settings) is hashed (`NetMatch::AssetsHash`). Peers with different files are refused at join time.
  Protocol changes need `kProtocolVersion` bumped (`src/networking`).
- **Screenshots**: there is no built-in facility. Temporarily add `SDL_RenderReadPixels` + `SDL_SaveBMP` before
  `m_renderer->Present()` in `Application::Render()` (works with the dummy video driver), then remove it.
- **Character editor** (`openaow-charactereditor`): the *Simulation* and *Compare* tabs try out unit stats
  without starting the game.
