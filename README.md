# Open Art Of War: Resistance Arisen

<div align="center">

<img src="docs/app-game-icon.png" alt="Open Art Of War: Logo" width="100" />

A native, open-source, **cross-platform real-time strategy game** recreating **Art of War 2: Liberation of Peru** for **Linux, Windows, macOS, Android and iOS**. Build your base, command the Resistance or Confederation, and play campaigns or skirmishes against the AI. **LAN and online multiplayer support up to eight players**, with teams, direct connections and dedicated server rooms.

[![Build](https://img.shields.io/github/actions/workflow/status/badcast/open-art-of-war/build.yml?branch=main&style=flat-square&label=build)](https://github.com/badcast/open-art-of-war/actions)
[![Platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20Windows%20%7C%20macOS%20%7C%20Android%20%7C%20iOS-informational?style=flat-square)](#platforms)
[![License](https://img.shields.io/badge/license-GPLv3-blue?style=flat-square)](LICENSE)

[Website](https://openaow.imister.tech) · [Downloads](https://github.com/badcast/open-art-of-war/releases) · [Report an issue](https://github.com/badcast/open-art-of-war/issues) · [Telegram](https://t.me/+EkC17x8nKy05NDdi)

<table>
<tr><td align="center">
<h3>Support development / Поддержать разработку</h3>
<p>Help support Open Art Of War with a donation on Boosty.<br>Поддержите разработку Open Art Of War донатом на Boosty.</p>
<a href="https://boosty.to/badcast/donate"><img src="https://img.shields.io/badge/Boosty-Donate%20%2F%20Поддержать-F15F2C?style=for-the-badge" alt="Donate on Boosty / Поддержать на Boosty"></a>
<p><a href="https://boosty.to/badcast/donate">boosty.to/badcast/donate</a></p>
</td></tr>
</table>

</div>

> **In active development.** The game is being developed and tested. A **fan-made asset pack is also in development**, with original graphics and UI artwork still being designed. Game artwork and audio are separate from the engine; see [Game assets](#game-assets) before playing.

![Open Art Of War main menu](docs/Screenshots/screen-mainmenu.png)

**On this page:** [Features](#features) · [Platforms](#platforms) · [Build & run](#build--run) · [Controls](#controls) · [Multiplayer](#multiplayer) · [Documentation](#documentation) · [Contributing](#contributing)

<details>
<summary>In Russian</summary>

**Open Art Of War: Resistance Arisen** - открытое воссоздание классической стратегии Art of War 2 для Linux, Windows, macOS, Android и iOS. Доступны кампании за две стороны, бои с компьютером, сетевая игра до восьми игроков, выделенный сервер ([`openaow-server`](openaow-server/README.md)) и редакторы.

> **Статус:** проект находится в разработке и тестировании. Графика и звук оригинальной игры не входят в лицензию движка. Перед запуском необходимо подготовить игровые ресурсы.

**Android и iOS:** добавлены альбомная ориентация, масштабирование интерфейса, крупные вкладки HUD и сенсорное управление. Инструкции по сборке и установке ресурсов находятся в [Android README](android-setup/README.md) и [iOS README](ios-setup/README.md).

**Как помочь:** кодом, тестированием, переводами, графикой и звуком. Задачи обсуждаем в Issues и чате Telegram - ссылки в начале страницы.
</details>

## Features

- **Two factions and campaigns:** Resistance - *Liberation of Peru*; Confederation - *Global Confederation*.
- **Skirmishes, multiplayer and dedicated server:** custom maps, computer opponents, teams and up to eight players; LAN discovery, direct connections, P2P NAT hole-punching and standalone headless [dedicated server](openaow-server/README.md) (`openaow-server`) rooms.
- **Classic RTS gameplay:** base building, power, economy, research, fog of war, attack-move and control groups.
- **Desktop and touch controls:** mouse and keyboard on desktop; touch selection, contextual commands, camera gestures and a landscape HUD on Android and iOS.
- **Configurable graphics:** zoom, quality settings, renderer fallback and shader effects where supported.
- **Creation tools:** map and character editors, configurable game data, mods and replay support.

<details>
<summary>More screenshots</summary>

| Gameplay | Maps |
| --- | --- |
| ![Game features](docs/Screenshots/screen-features.png) | ![Map selection](docs/Screenshots/screen-maps.png) |
| ![Multiplayer lobby](docs/Screenshots/screen-network-play.png) | ![Multiplayer map](docs/Screenshots/screen-network-skulmap.png) |

</details>

## Platforms

| Platform | Build and interface |
| --- | --- |
| Linux | Native desktop build with mouse and keyboard |
| Windows | Native desktop build with MSVC or MinGW |
| macOS | Native desktop build |
| Android | Android 7.0+; ARM64 and x86_64; fullscreen landscape with touch controls |
| iOS | iOS 15.0+; ARM64 (device and simulator); Xcode / CMake static build with Metal shaders and touch controls |

The Android project is in [`android-setup/`](android-setup/README.md), and the iOS setup is in [`ios-setup/`](ios-setup/README.md). Common touch-device code uses `OAOW_TOUCH`, set by CMake for supported mobile builds; platform-specific calls are isolated in `platform/TouchPlatform.cpp`.

## Build & run

### Game assets

The engine needs game data, artwork and audio. Original Art of War 2 assets belong to Gear Games and are **not covered by the engine's GPLv3 license**. A **fan-made asset pack is in development**: its graphics and UI artwork are still in the design phase, and it is not yet a complete, ready-to-play replacement. Until it is ready, use assets from your legally obtained copy of the original game or another compatible asset pack.

On desktop, place the prepared `assets/` directory in the repository root, or put `assets.blob` beside the executable. The default archive name is **`assets.blob`**.

To pack an existing `assets/` directory during a desktop build, add `-DBUILD_ASSETS_BLOB=ON` to the configure command. Android and iOS install bundled blobs from the package on first launch or read `assets.blob` from their application storage directory; see the [Android asset instructions](android-setup/README.md#game-assets) and [iOS asset instructions](ios-setup/README.md#game-assets-game_data_urls).

### Desktop dependencies

The project uses a C++17 compiler, CMake and SDL3. Install the development packages for your platform:

| Platform | Dependencies |
| --- | --- |
| Arch Linux | `base-devel cmake ninja sdl3 sdl3_ttf yaml-cpp nlohmann-json gettext libarchive zstd pkgconf python` |
| Windows / MSVC | vcpkg packages: `sdl3 sdl3-ttf yaml-cpp nlohmann-json gettext-libintl libarchive[zstd] pkgconf` |
| Windows / MinGW | MSYS2 MINGW64 packages: `toolchain cmake ninja SDL3 SDL3_ttf yaml-cpp nlohmann-json libarchive zstd gettext-runtime gettext-tools` |
| macOS | Homebrew packages: `cmake ninja sdl3 sdl3_ttf yaml-cpp nlohmann-json gettext libarchive zstd pkgconf spirv-cross` |

Networking, debugging UI and audio-decoding dependencies are bundled in `3rdParty/`. Shader compilation uses `glslc` when available, otherwise the precompiled shaders are used. Platform-specific toolchain setup is recorded in the [desktop build workflow](.github/workflows/build.yml).

### Build the desktop game

From the repository root, after installing dependencies and configuring your compiler environment:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/openaow
```

On Windows, run `build\openaow.exe`. MSVC builds also need the vcpkg toolchain passed at configuration time; see the workflow above.

The desktop build also produces `openaow-server`, `openaow-mapeditor` and `openaow-charactereditor`. Use `-DOAOW_BUILD_SERVER=OFF` if the server is not needed.

### Build for Android

Open `android-setup/` in Android Studio, or build with an installed Gradle, JDK 17, Android SDK and NDK:

```bash
cd android-setup
gradle assembleDebug
```

The APK is written to `android-setup/app/build/outputs/apk/debug/` relative to the repository root. Install it and provide `assets.blob` separately.

See the [Android setup guide](android-setup/README.md) for prerequisites and asset installation. The [Android build workflow](.github/workflows/android.yml) contains the CI setup, including SDL Java sources.

### Build for iOS

On macOS with Xcode 15+, CMake 3.22+, Git and `spirv-cross` installed (`brew install cmake ninja spirv-cross`), run the helper script from the repository root:

```bash
./ios-setup/build-ios.sh device --ipa   # Unsigned device .app + OpenArtOfWar-ios.ipa
./ios-setup/build-ios.sh simulator       # iOS Simulator build
```

All iOS dependencies (`SDL3`, `SDL3_ttf`, `yaml-cpp`, `nlohmann_json`, `zstd`, `libarchive`) are fetched and built statically via [`cmake/IosDeps.cmake`](cmake/IosDeps.cmake), and `GAME_DATA_URLS` assets are downloaded into `ios-setup/assets/` when set. See the [iOS setup guide](ios-setup/README.md) and [iOS build workflow](.github/workflows/ios.yml) for signing, simulator usage and Finder/Files asset sideloading.

### Useful launch options

```bash
./build/openaow --map=m26                 # Start a custom game
./build/openaow --map=m26 --from-scratch  # Start with HQ and power plant
./build/openaow --testmap                 # Open the test map
./build/openaow --replay=match.awr         # Watch a replay
./build/openaow --renderer=opengl         # Override the renderer for this run
./build/openaow --help                    # List all options
```

## Controls

### Desktop

| Action | Input |
| --- | --- |
| Select units | Left-click or drag a selection box |
| Move / attack / set rally point | Right-click |
| Assign / recall a control group | `Ctrl+1..0` / `1..0` |
| Attack-move | `Q`, then left-click the destination |
| Toggle siege mode | `G` |
| Move camera | `W A S D`, arrow keys or middle-button drag |
| Zoom map | Mouse wheel over the map |
| Move camera using radar | Click or drag on the radar |
| Zoom radar | Radar `+` / `−` buttons or mouse wheel |
| Build, recruit, research, issue orders | Command sidebar |
| Open game menu | `Escape` |
| Toggle fullscreen | `F11` or `Alt+Enter` |
| Pause a network match | `P` or `Pause` |

### Touch devices (Android & iOS)

| Action | Gesture |
| --- | --- |
| Select a unit / activate a button | Tap |
| Select several units | Drag with one finger on the map |
| Move / attack / set rally point | Hold for at least 0.5 seconds, then release |
| Cancel a production order | Hold its production card, then release |
| Move and zoom the camera | Drag or pinch with two fingers |
| Switch HUD tabs | Tap the labelled sidebar buttons |
| Back / game menu | On-screen `MENU` button (or system Back button on Android) |

The interface scales with the screen and stays in landscape orientation. Camera gestures are disabled while menus are open.

## Multiplayer

Choose **Network game** from the main menu and enter a player name.

| Connection | How to play |
| --- | --- |
| LAN | Create a game or join one found on the local network |
| Direct connection | Enter the host's address, optionally with `:port`; the host needs an accessible UDP port |
| Server room | Create or join a room through the configured server; port forwarding is not needed |
| Join code | Enter the room's six-character code |

The host chooses the map and computer opponents. Players choose their faction, team, colour and start position, then mark themselves ready. Missing maps are transferred through the lobby.

Opening the game menu does not pause a network match. Network matches run in **Authority/Replica** mode by default (with deterministic lockstep fallback via `OAOW_LOCKSTEP=1` and for replays) and support seamless reconnecting. In Authority/Replica mode, the room owner simulates the world and streams real-time **world snapshots** (`WorldSnapshot`) optimized specifically for low-latency RTS transmission:

- **Per-player fog-of-war filtering** and **ack-tracked delta encoding** (`AuthorityAck`) send only visible, changed entities, walls, and transient combat events.
- **Compact binary encoding** quantises unit sub-tile positions to `1/16 px` steps and packs fields with varints before compression.
- **Tuned Zstandard (`zstd`) compression** (`ZSTD_compress2` at `ZSTD_CLEVEL_DEFAULT` with frame checksums, content-size headers, a reused `thread_local` compression context, and bounded `2 MiB` single-frame decode) cuts snapshot traffic roughly 4.5x on full keyframes while keeping authority CPU overhead minimal; ENet also adds CRC-32 datagram integrity and range-coder wire compression, while `openaow-server` forwards compressed snapshot payloads directly without re-encoding.

### Dedicated server (`openaow-server`)

The repository includes a standalone headless **dedicated server** (`openaow-server`) that provides the public room directory, on-demand dedicated game rooms, P2P NAT hole-punching rendezvous (preserving local/mobile hosting), HTTP JSON status monitoring and `.awr` match replay recording—requiring no graphics, audio or game assets. See the full [Dedicated Server Guide](openaow-server/README.md) for CLI flags, `--config` JSON options and deployment notes.

<details>
<summary>Command-line examples and dedicated server</summary>

```bash
# Host and join a LAN game
./build/openaow --host=38100 --players=2 --map=m26 --name=Alice
./build/openaow --join=192.168.1.10:38100 --name=Bob

# Create or join a server room
./build/openaow --server=openaow.imister.tech --server-create --players=2 --map=m26
./build/openaow --server=openaow.imister.tech --server-code=ABC234

# Run a dedicated server
./build/openaow-server --name "My server"
./build/openaow-server --help
```

The server needs no graphics or game assets. Default ports are UDP `38100` for discovery and room access, UDP `38110–38199` for rooms, and TCP `38100` for the HTTP status endpoint. See the [Dedicated Server README](openaow-server/README.md) and [systemd service example](openaow-server/openaow-server.service) for Linux hosting.

Network matches use Authority/Replica state synchronization by default (with deterministic lockstep fallback). Network defaults are configured in `assets/data/game_environment.yaml`.

</details>

## Settings & saved data

Open **Settings** from the main menu or game menu to adjust graphics, audio, language and zoom. Desktop builds also offer window modes and display resolutions; Android and iOS use the fullscreen device surface.

The game stores preferences and progress in its per-user application directory, separate from the factory data in `assets/`:

| File or directory | Purpose |
| --- | --- |
| `settings.json` | Preferences, player details and renderer settings |
| `savegame.json` | Campaign progress |
| `downloaded_maps/` | Maps received in multiplayer |
| `replays/` | Recorded network games |

## Documentation

| Guide | Contents |
| --- | --- |
| [Dedicated server](openaow-server/README.md) | Headless dedicated server (`openaow-server`), CLI/`--config` flags, ports, HTTP status and replays |
| [Android](android-setup/README.md) | Android build setup, assets, touch controls and device checks |
| [iOS](ios-setup/README.md) | macOS/Xcode build setup, Metal shaders, `GAME_DATA_URLS` and sideloading |
| [Tests & debugging](TESTS.md) | Test suites, debug builds and diagnostic tools |
| [UI layout](docs/ui-layout.md) | `assets/ui/layout.yaml` profiles (`pc`, `touch`) and live reload |
| [Map editor](openaow-mapeditor/README.md) | Map creation and editing |
| [Character editor](openaow-charactereditor/README.md) | Unit and building configuration |
| [Modding](docs/modding.md) | Custom game content and tech levels |
| [Replay format](docs/replay-format.md) | Replay file structure (`.awr`) |
| [CI game data](docs/ci-game-data.md) | `GAME_DATA_URLS` bundling for desktop, Android and iOS CI builds |

Run tests after building:

```bash
ctest --test-dir build --output-on-failure
```

Tests do not generally require the in-game debugger; some need game assets. To enable debug tools, configure a **separate** build:

```bash
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Release -DAOW_DEBUG=ON
cmake --build build-debug --parallel
./build-debug/openaow --debug
```

## Contributing

Contributions are welcome in gameplay, networking, mobile (Android & iOS) support, UI, testing, translations, original artwork and audio.

Start with an issue describing the problem or proposal, then submit a focused pull request with verification steps. For bugs, include your platform, build version, reproduction steps and relevant logs. Use the community links at the top of this page to discuss work with other contributors.

<details>
<summary>Why this project exists</summary>

Art of War 2 was a favourite on classic Nokia phones. Inspired by Command & Conquer, OpenRA and Warzone 2100, this project brings that portable RTS experience to modern systems while keeping its base building, factions and tactical gameplay.

Thanks to the original creators and to everyone helping give the game a second life.

</details>

## License & credits

The engine, tools and editors are licensed under [GNU GPLv3](LICENSE). Bundled third-party libraries retain their own licenses.

**Art of War 2: Liberation of Peru**, its original code, artwork, audio and trademarks belong to **Gear Games**. This is an independent recreation and is not affiliated with or endorsed by Gear Games. Original proprietary assets are not covered by this project's source-code license.
