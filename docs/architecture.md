# Art of War 2: Liberation of Peru - Reverse Engineering & Architecture Specification

## 1. Executive Summary
- **Original Entry Point**: `aow22.class` (MIDP-2.0 / CLDC-1.0 profile, vendor Gear Games)
- **Target Application**: Modern Native Linux Desktop Application in C++17/20 (SDL3, SDL3_ttf, Dear ImGui, yaml-cpp, nlohmann/json, stb_vorbis)
- **Internal Resolution**: 240x320 (original virtual screen), scaled to modern display with free-camera Pan & Zoom.
- **Engine Loop**: Fixed timestep simulation (20 TPS in gameplay = 50ms interval, 10 TPS in menus = 100ms interval).

---

## 2. Deobfuscation & Class Symbol Mapping

| Obfuscated Class | Deobfuscated Name | Primary Purpose / Role | Key Methods & Characteristics |
| :--- | :--- | :--- | :--- |
| `aow22` | `GameApp` | J2ME MIDlet entry point & lifecycle manager | `startApp()`, `pauseApp()`, `destroyApp()`. Instantiates `EngineCore` and manages root `Display`. |
| `a` | `EngineCore` | Core Engine Canvas & Main Game Loop (`Canvas`, `Runnable`) | `run()` (game loop), `paint()` (frame output), `keyPressed()`, `keyReleased()`, `pointerPressed()`, `pointerDragged()`, `pointerReleased()`, `loadLevelData()`, `loadTileSprites()`, `loadUnitSprites()`, `initAudio()`, `initGraphics()`. Manages 270x340 backbuffers. |
| `f` | `GameWorld` | RTS Game World, HUD, UI Menus, Campaign Controller, RMS Persistence | 11,500 lines, 139 methods. Manages game states (Menu, Campaign, Skirmish, Briefing), minimap rendering, build orders dispatch, technology tree, save/load to RMS. |
| `i` | `SimulationCore` | RTS Simulation Engine, Unit Entity Table, Combat, Pathfinding & Fog of War | 5,750 lines, 79 methods. Flattened Structure-of-Arrays (`f1150S`, stride 101) for up to 100 simultaneous units. Real-time combat, ballistics, line-of-sight fog-of-war, A* grid pathfinder. |
| `e` | `BitmapFont` | Bitmap Font Renderer for fonts `/s` (small), `/t` (title), `/d` (dialog) | Glyph parsing, text layout and measurement, colored string rendering with shadow/highlight support. |
| `d` | `SoundManager` | Audio Engine (MIDI Soundtrack & Sound Effects) | J2ME `PlayerListener`. Reads `/s0` MIDI bank (3 tracks: 10550, 7710, 8424 bytes) and WAV sound effects with volume control. |
| `g` | `PlatformConfig` | Device profile & platform metadata | Vendor links, platform specific key codes, localization string helper. |
| `c` | `GameConstants` | Balance tables, Unit/Building/Weapon archetype constants, Color palette | Static constants, color palette LUT (`f569a`). |
| `h` | `ResourceOffsets` | Asset sizes, binary offsets, glyph indices | Static offsets for `/0/d0` (19703 bytes), `/0/s0` (1546 bytes), etc. |
| `b` | `BluetoothNetwork` | Bluetooth multiplayer networking | J2ME `DiscoveryListener`, RFCOMM/L2CAP transport for 2-player matches. |
| `j` | `HttpReporter` | HTTP error and crash reporting client | Reports crash dumps and scores to Gear Games server. |
| `k` | `SmsHandler` | SMS monetization / billing client | Legacy J2ME carrier billing (bypassed in native port). |

---

## 3. Unit Entity Data Model (Flattened SoA Layout)
In `EngineCore`, `SimulationCore`, and `GameWorld`, unit entities are stored in a contiguous 1D array indexed with stride `101`:
`unitArray[FIELD_INDEX * 101 + unitId]` (for unitId 1..100):

| Offset | Field Name | Description |
| :--- | :--- | :--- |
| `0` | `tileX` | Tile X coordinate on map (0..127) |
| `101` | `tileY` | Tile Y coordinate on map (0..127) |
| `202` | `subTileX` | Fine sub-tile pixel offset X (0..29) |
| `303` | `subTileY` | Fine sub-tile pixel offset Y (0..29) |
| `404` | `targetTileX` | Order destination Tile X |
| `505` | `targetTileY` | Order destination Tile Y |
| `1010` | `targetEntityId` | Targeted enemy unit or building ID |
| `1414` | `maxHp` | Maximum Hit Points |
| `1616` | `currentHp` | Current Hit Points |
| `2323` | `unitTypeId` | Unit Archetype ID (Infantry, Tank, Turret, etc.) |
| `4747` | `facingDirection` | Facing orientation (0..15 angles) |
| `4848` | `actionState` | State FSM: 0=Idle, 1=Move, 2=Attack, 3=Die, 4=Build, 5=Harvest |
| `5454` | `factionId` | Faction Owner: 0=Resistance (Player), 1=Confederation (AI) |

---

## 4. Binary Resources Overview
- `/s0` (26,684 bytes): MIDI Soundtrack containing 3 tracks (Menu theme, Combat theme 1, Combat theme 2).
- `/d0` (103,215 bytes): Audio sound effects bank (WAV/PCM samples).
- `/i0` (282,547 bytes): Sprite atlas & unit animations.
- `/l1`, `/l2`: PNG Splash screens & Title art (Gear Games logo, Cover art).
- `/r00` .. `/r05`: PNG Terrain tile atlases (98x97 pixels each).
- `/m0`, `/m26`, `/m35`, `/m37`, `/m42`, `/m43`, `/m44`: Level map binaries.
- `/mi0` .. `/mi7`: Mission info, briefings, scripting triggers.
- `/0/s0`, `/0/t0`, `/0/d0`: Font glyph metrics and textures.
- `/n`: Localization string table.
- `/a`: Unit stats, weapons, economy balance configuration.

---

## 5. Port Gameplay Systems
- **Sides** (`game/Sides`): `playerFaction()` / `aiFaction()`, chosen in the mission dialog (Resistance or Confederation).
  Map data keeps its owners; playing the Confederation swaps the player / enemy bases.
- **Production** (`game/Economy`): every Barracks / Factory owns a queue (`Building::productionQueue`) and a rally
  point; units appear at that building's exit only. RMB on a unit card cancels one order with a refund.
- **Control groups** (`GameplayController`): Ctrl+1..0 assigns, 1..0 selects (twice = camera). A move order to a
  group runs through `AISquadController` (leash, chokepoints) and dissolves at the destination.
- **Research** (`game/Research`): one 8-technology catalog per faction; requires a finished Tech Center /
  Laboratory (paused otherwise). The AI researches in order, the player in any order. See `docs/research/techs.html`.
- **AI attack groups**: a wave splits into 1..5 groups (`max_attack_groups`, `credits_per_extra_group` in
  `difficulty.yaml`); extra groups march through a flank point or the second base exit.
- **Skirmish base builder**: wall arc facing the player with two exits, towers / bunkers inside it, lab and power
  plants behind the HQ, new towers toward the side the player attacks from (`build_defenses`).
- **Fog of war**: soft rounded edges from a blurred per-frame mask; look set in `assets/data/game_environment.json`
  (`fog.rounding`, `texels_per_tile`, `explored_alpha`, `hidden_alpha`, `color`).
- **Construction rule**: no building while a visible enemy unit / structure is within `construction.enemy_block_tiles`
  (6) tiles of the footprint (`assets/data/game_environment.json`). Own structures can be blown up from the sidebar
  ("Destroy", second click confirms); queued orders are refunded.
- **Mission scripts**: command delays are relative to the previous command (as in the original), so waves follow the
  script's timeline. Defense missions stretch it by the difficulty (`defense_first_wave_seconds`,
  `defense_wave_gap_seconds`, `defense_squad_percent`, `defense_time_scale` in `difficulty.yaml`).
- **Radar**: zoom x1..x4 (wheel or +/-), hides unexplored ground, enemy units out of sight and unexplored enemy
  structures (off with the debug fog reveal); colors follow the faction, not "own / enemy".
- **Draw order**: walls, buildings and units are sorted by the bottom edge of what they stand on, so tall parts of
  a structure cover the units behind it; bars are drawn after all sprites.
