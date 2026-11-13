# TASKNEW — authoritative host, replica clients

Goal: replace lockstep (every peer simulates, checksums compared) with **state replication**: the host (room owner,
`SessionHost::m_authoritySlot`) simulates the world, clients send commands and receive state.
Why: float results differ between machines/builds (see `DESYNC/`, desyncs at ticks 60/120/160 on a nearly empty
world), and every new simulation change risks new desyncs.

Legend: `[x]` done, `[ ]` to do, `[~]` in progress.

## Decisions

- Authority = room owner at match start; never transferred (host leaves = match ends, no host migration).
- Clients do not call `SimulationStep` (`Simulation::ExecutionMode::Replica`); the host runs `Authority`.
- Commands: client → host over the existing `Commands` channel; the host applies them on the next tick.
- State goes per player (`AuthorityState`, addressed by `recipient`) because each player has its own fog of war.
- Snapshot rate 10 Hz (sim stays 20 ticks/s); the client interpolates between snapshots.
- Visual events (explosions, shots, wrecks, craters, sound cues, camera shake) are sent as events, never feed any checksum.
- Dedicated server (`openaow-server`) only relays; ask the user before redeploying it.
- Lockstep path stays for replays and headless determinism tests until step 8.

- Found in `NetMatch.cpp` (`CanRunTick`, `ScheduleBundle`, `AfterTick`): `SessionHost` is only a relay of turns, the game
  runs on the clients (also on the dedicated server). So the authority is the **room owner's game client**, not
  `SessionHost`. Minimal path for step 1: keep `Commands` → `TurnBundle` as the command channel; only the authority
  client consumes bundles (`CanRunTick`/`ScheduleBundle`), replicas ignore them and only send commands.
  `AfterTick` on replicas must not send `WorldHash`.

## Open questions

- [ ] Keep lockstep as a mode (replays, `openaow-determinism-tests`) or remove it completely after step 8?
- [ ] Find the root cause of the current desyncs first? (needs the dump of the second side; only slot 0 dumps exist)

## Codex review of step 0 (read-only, findings to fix)

- [x] `AuthorityState` rides `Bulk`, `StartMatch` rides `Lockstep`: no ordering; an early snapshot is wiped by the start
      handler (`SessionClient.cpp:111`). Add a match id / start counter to the snapshot and drop foreign ones.
- [x] `sendAuthorityState` refuses pre-match, self-addressed, oversized, and locally known missing/disconnected recipients. Boundary, truncation, and real ENet fragmentation of a 2 MiB snapshot pass. A successful send is not an end-to-end delivery acknowledgement.
- [x] The authority slot is announced in `StartMatch`; the match ends when the authority is given up. Covered by network tests.
- [x] Replica guard now also in `Simulation` (ApplyCommands, UpdateEconomies, UpdateOrderQueues, UpdateResearch, UpdateSquads).
- [x] (now 2, `matchId` added to `StartMatch`/`AuthorityState`) `kProtocolVersion` is still 1: bump with the first network-visible change, not at step 8.
- Desync suspects: `Hypot` normalisation in movement (`Unit.cpp:442`), float bits hashed in `Simulation.cpp:63,776`;
  `DetMath::Hypot` change in the working tree already removes `std::hypot`. FMA is off for GCC/Clang only
  (`CMakeLists.txt:29`), not MSVC. Need paired dumps of the same tick + build flags of both machines.

## Codex review of steps 1-5 (read-only, gpt-6.1-terra) — to fix

High:
- [x] (`MatchSetup::authorityMode`, the room owner decides; verified live with the joiner lacking the switch) Authority mode is read locally (`ApplicationNetwork.cpp`, setting / `OAOW_AUTHORITY`) and never agreed at match start: mixed settings = lockstep client next to a replica. Put the mode into the match setup so everybody follows the room owner.
- [x] (dead and dying entities are not sent, the replica erases what is gone, spawns without the slot limit; a failed spawn is still only `false`) `ApplySnapshot` spawns before it kills the missing ones (`FreeEntitySlots` can refuse), ignores a `false` result, and dead entities stay forever (no `Update` on a replica): erase the entities a snapshot drops, spawn without the slot limit, handle failure.
- [x] (a queue of 64, every snapshot is delivered in order) `SessionClient` keeps one pending snapshot: a second one overwrites the first and its one-time events (explosions, sounds, shake) are lost. Queue them, or put the events in a separate stream that is never dropped.
- [x] The snapshot tick filter resets per match and advances only after the application decodes, applies, and acknowledges a snapshot; invalid high-tick payloads cannot suppress later valid snapshots.
- [x] (`EntityManager::UpdateVisualEffects` runs on replicas) Replica explosions are never animated or removed (the update that does it does not run on a replica): run the visual-only update (explosions, craters, wrecks) on replicas.

Medium:
- [~] Finished projectiles are removed after their update (live-shot order is preserved); `FreeEntitySlots` remains linear per spawn (quadratic for 500 entities).
- [x] (`m_replicaSentTurn` paces replica sends at most once per computed target turn so rapid commands coalesce; `m_replicaSentTurn`/`m_lastTickMs` and `Application` per-match caches reset across matches, and `NoteTickRan` updates on replica snapshot apply) Replica commands are timed by the last received bundle; `m_replicaSentTurn` is written but unused. Host moves late commands to the next open turn, so nothing is lost, only delay varies.
- [x] (class / archetype mismatch replaces the entity) Match entities by id only: check class / archetype too, replace on mismatch; a failed spawn leaves a half-applied snapshot; `id + 1` overflow.
- [x] Snapshot entities are filtered by the recipient team’s current visibility; enemy economies are omitted.
- [x] A forged `AuthorityState` from a normal client is refused by the host (slot + matchId checked). The decoder validates snapshot field ranges, finite visual/research floats, and sorted unique entity/delta/hidden IDs.

## Helpers

- Codex (`gpt-6.1-terra`, read-only) works through `codex exec`.
- agy (`gemini-4-hydrogen-high`) needs `--dangerously-skip-permissions` in headless `-p` mode (the user allowed it). It wrote the AuthorityState boundary / non-authority / authority-timeout tests in `NetTests.cpp` (2380 checks pass).

## Step 0 — groundwork (already in the working tree, uncommitted)

- [x] `Simulation::ExecutionMode { Standalone, Authority, Replica }`, `AdvancesWorld()`
- [x] `GameplayController::SimulationStep` returns early for replicas
- [x] Protocol: `MsgType::AuthorityState`, `AuthorityState {recipient, tick, bytes}`, `kMaxAuthorityStateBytes`
- [x] `SessionHost` relays `AuthorityState` only from `m_authoritySlot`; `SessionClient::sendAuthorityState/takeAuthorityState`
- [x] Net tests for the relay (`src/networking/tests/NetTests.cpp`)
- [x] Groundwork committed in `5d4d1de`; subsequent changes and test coverage are tracked separately.

## Step 1 — roles in `NetMatch`

- [x] Host sets `Authority`, clients set `Replica` at match start (`NetMatch::Attach`; opt-in setting `network.authority_mode`, default off until step 5; `StartMatch.authoritySlot` tells every peer who simulates)
- [x] Clients send their commands to the host and do not schedule them locally (`NetMatch::PumpReplica`, replica branch in `Application.cpp`)
- [~] Host applies received commands on its next tick (still via `TurnBundle` consumed by the authority only; dropping input-delay turns is a later cleanup)
- [x] AI players, missions, victory/defeat run only on the host; team-based `result` is filtered and sent to clients (`AuthorityMatchContinues` keeps 3+ team authority hosts ticking after own defeat)
- [x] Test: loopback peers (`ReplicaLoopbackTests.cpp`), host simulates, client world stays untouched without snapshots, full + delta, 3-team elimination, `PlayerLeft`, and reconnect recovery

## Step 2 — snapshot format (`src/net/WorldSnapshot.cpp`, header in `include/net/`)

- [x] Define the content: entities (id, type, owner, tile + sub-tile offset, hp, state, facing, turret, target, orders,
      production queue), economies, research, mines, walls/map changes, removed ids
- [~] `WorldSnapshot::CaptureEntities(EntityManager&, tick)` done; todo: economies (owned by `Simulation::m_economies`, needs an accessor), research, mines, walls, viewer filter; `Apply` is step 3
- [x] Version number in the header; unknown version = refuse
- [x] Round-trip test (`tests/WorldSnapshotTests.cpp`, ctest `openaow-worldsnapshot-tests`): world → snapshot → fresh world → snapshot must be byte-identical
- [~] Coordinates quantised to 1/16 px and varints used; zstd compression still to do

## Step 3 — entities accept state from outside

- [x] `EntityManager::ApplySnapshot` (`src/game/EntityManagerReplica.cpp`): create / update / delete by id without running behaviour code (economies via `Simulation::ApplyEconomies`, walls via `WorldSnapshot::ApplyWalls` (snapshot v3, sent on change and once a second), fog recomputed on the replica; mines, research, rubble/craters still missing)
- [ ] Replica never runs `Unit::Update`, combat, economy, AI, pathfinding
- [~] `ApplySnapshot` calls `SnapshotRenderState` first; render-time interpolation factor for replicas is step 5
- [~] Test: apply to an empty world, follow a moving/damaged entity, kill by omission (`WorldSnapshotTests.cpp`); a running-skirmish comparison is still to do

## Step 4 — transport of snapshots

- [~] Per-recipient acknowledged snapshot history and deltas implemented; build and runtime verification pending.
- [~] Replica acknowledges successfully applied snapshots; authority sends periodic full snapshots and falls back when the base is missing or too old. Reconnect needs live verification.
- [x] Reliable ENet fragmentation reassembles a maximum 2 MiB authority snapshot over local UDP; larger snapshots are rejected by the existing limit.
- [ ] Unreliable/sequenced channel for deltas, drop outdated ones (client already ignores `tick <= last`)
- [ ] Tests in `NetTests.cpp`: loss, reorder, late joiner, reconnect

## Step 5 — client rendering

- [x] Interpolate positions between the two last snapshots at render time (`m_replicaSince`, `UpdateRenderAlpha`: glide over the 2 ticks between snapshots)
- [~] Shots in flight, explosions and building blasts are carried (snapshot v2: `CaptureEffects`, `EntityManager::TakeReplicaEvents`); wrecks and craters still to do
- [~] Sound cues and building blasts (camera shake) are carried; the announcer / voice lines are still to check
- [ ] HUD, radar, selection, order lines, build menu work from the replica world
- [ ] Local feedback while waiting for the host: unit reply sound, target marker, build ghost

## Step 6 — fog of war and traffic

- [~] Host filters entities by recipient visibility and hides enemy economies; verification pending. Walls and visual events retain their existing behavior.
- [~] Entering vision sends the entity; leaving vision retains known hidden entities on the replica, including remembered buildings. Verification pending.
- [~] Measured (live skirmish, uncompressed full snapshots): ~29 bytes per entity, 35 entities = 1013 B per snapshot = ~10 KB/s per player. Extrapolated to 500 entities ~14 KB per snapshot = ~140 KB/s per player: too much, so deltas (send only changed entities) and zstd are needed. Then tune rate and quantisation (target 20–50 KB/s)
- [x] Stats overlay (`ApplicationOverlays.cpp`, `NetMatch::Stats`) shows snapshot size, rate and age in authority mode

## Step 7 — leave, reconnect, pause, replays

- [x] Player leaves: host applies `PlayerLeft` locally, clients get the result in the next snapshot (loopback test in `ReplicaLoopbackTests.cpp`)
- [x] Reconnect = full snapshot, no replay of past turns (loopback test in `ReplicaLoopbackTests.cpp`)
- [~] `PauseState` blocks already-buffered authority turns and freezes replica interpolation; targeted tests and live verification remain
- [ ] Replays: the host records input commands as now and plays them in `Authority` mode; update `docs/replay-format.md`

## Step 8 — clean up

- [~] Authority/replica matches no longer send `WorldHash`; lockstep still uses hashes and desync dumps until its future is decided.
- [ ] Simplify `SimBuildId` / `DataSync` for clients (only the snapshot version must match; visual data check stays)
- [ ] Bump `kProtocolVersion`
- [ ] Update `CLAUDE.md` (network section) and `docs/`
- [ ] Ask the user before redeploying `openaow-server` (openaow.imister.tech)

## Live check (2026-11-13)

Two headless instances (`SDL_VIDEODRIVER=dummy`, AOW_DEBUG build in a scratch copy outside the repo), `--host=PORT --players=2
--map=skirmish_1` and `--join=127.0.0.1:PORT`, with `OAOW_AUTHORITY=1 OAOW_NET_BOT=1 OAOW_NET_TRACE=1`: 1000 ticks with bot
production and building, 50 of 50 snapshot hashes printed by the authority (`[Snap] auth`) equal the hashes of the replica's
own re-capture (`[Snap] repl`). This proves entities + economies are carried exactly; it does not prove that the picture is
right (interpolation, events, walls, mines, fog are not carried yet).

Live check 2 (delta snapshots + fog filter, same setup): 51 of 51 hashes equal (the authority hashes what it sent to the recipient after
the filter, the replica hashes what it holds of exactly those entities). Traffic ~1.9 KB/s per player at 27 entities (was ~10 KB/s as
full snapshots); the replica saw 17 of 27 entities (the rest hidden by fog).

## Screenshot check of the replica (headless, dummy video driver)

The sidebar (credits, power, minimap with units, build menu) of the replica matches the authority's. The world view is black in
this headless setup for the authority and for a plain lockstep client alike, so the world picture of a replica (units, buildings,
interpolation, effects) has still not been seen by anyone: it needs one real run on a screen. Found and fixed with it: the replica
showed "Catching up with the match: 0%" (`NetMatch::TurnsAhead` now returns 0 for a replica).

## Verification checklist (after every step)

- [ ] `cmake --build build -j16` and `ctest --output-on-failure` pass
- [ ] `openaow-determinism-tests` (AOW_DEBUG build in a separate dir) still pass
- [ ] Two real instances (`--host` / `--join=`) play a few minutes without drift
- [ ] New user-visible strings added to `po/openaow_ru.po` and `po/openaow.pot`, `msgfmt -c` clean

## Risks

- Order latency = RTT + up to 100 ms; hide with local feedback (step 5).
- Host upload = (N−1) streams; host CPU = simulation + serialisation.
- Host leaves = match ends.
- Largest unknown: making `EntityManager` accept external state (step 3).

## Snapshot delta and fog implementation (2026-11-13)

Snapshot v5 includes hidden-alive IDs so full snapshots preserve previously seen entities under fog. The authority retains per-recipient history, uses acknowledgements for delta bases, and reports snapshot payload bytes with `OAOW_NET_TRACE`. The replica expands deltas against the acknowledged base and acknowledges only after application. Protocol version is 3; the relay must be updated before mixed builds can connect. Unit tests were extended, but CMake could not create the requested build directory under `/home/badmister/.cache/oaow-e2e/helpers/codex-build`: it is outside the sandbox writable roots. Build, ctest (excluding NetTests), and the user’s live check remain pending.

## Research replication in progress

- [~] Snapshot v6 carries allied research and progress, with delta filtering and periodic refresh; replica applies completed effects and progress. Snapshot tests extended; full build, all 17 tests and the live two-instance check (51 of 51 hashes, ~1.9 KB/s) pass. Exact restoration when completed research is removed has not been implemented or verified (a research can only grow in a match, low priority).
- [x] Mines: own and allied mines ride on the snapshot (v7); hostile mines are never sent (by design the other side never sees them; Codex had made them visible on lit tiles, reverted).
- [x] Match result: the authority computes team-based victory / defeat per recipient (`WorldSnapshot::result`, snapshot v8), the replica switches to Victory / Defeat. Verified in `WorldSnapshotTests` and 3-team `ReplicaLoopbackTests`.
- [x] Pause: nothing to do: the host stops sending turns while paused, so the authority cannot tick (a Codex attempt that refused ticks on every client was reverted after review: it froze the UI in the catch-up branch and could stall).
- [x] Player leave & reconnect: `ScheduleBundle` applies `PlayerLeft` on the authority; a connected -> disconnected roster change resets the peer so a returning replica gets a full snapshot without replaying past turns (verified in `ReplicaLoopbackTests`).

## Replica death visuals (snapshot v9)

- [~] Death events carry the authority's death animation and wreck state, and replicas age visible remains without simulation. Explosions and blasts use the existing event stream. Snapshot round-trip and replica behavior tests pass in the targeted Release build; the test reports no loaded sprite or character configs, and live visual verification is pending. Projectile craters are carried as visual events.

## Codex audit of the replica path (2026-11-13)

- [x] `NetMatch::CanRunTick/ScheduleBundle/AfterTick` refuse on a replica, `Detach` restores Standalone, replica research comes from the snapshot.
- [~] `Attach` sets the role before match loading and replicas skip AI initialization. The initial map and starting world are still built on replicas before the first snapshot.
- [x] `OAOW_NET_BOT` acts once per tick (`DriveNetworkBot`).
- [x] `Simulation::Schedule` and `EndTick` refuse direct calls in Replica mode; player commands still reach the network sink. Covered by the replica loopback test.
- [x] `tests/ReplicaLoopbackTests.cpp` (agy): host ticks, replica stands still without snapshots, full + delta snapshots match (ctest openaow-replica-tests).

## Touch build (2026-11-13)

Clip rects broke behind SDL's logical presentation on devices (empty radar, cut dialogs): now a plain render scale, ImGui in those units, touch confirm drawn by the game. Needs a check on a phone.

## Verification update

Release build completed; all 18 CTest tests pass when the ENet test is run with local socket access (the sandboxed ENet run cannot open its sockets). Snapshot validation now rejects non-finite visual/research values and unsafe entity IDs; other fields still require validation. A separate `AOW_DEBUG` determinism build passes. Two headless local instances (authority and replica with network bots) produced matching re-captured snapshot hashes at 61 of 61 sampled ticks through tick 1200. This does not replace a real visual multiplayer check.

## Match-end follow-up

- [x] The authority publishes a final snapshot immediately when victory or defeat occurs (including odd ticks), evaluates `FilterForPlayer` per team, and continues simulating/streaming snapshots via `AuthorityMatchContinues` in 3+ team matches even if the host player is eliminated first. Covered by `WorldSnapshotTests` and `ReplicaLoopbackTests`.
