# Open Art of War — iOS Build Setup

This directory contains the iOS configuration for **Open Art of War**, built with CMake, Xcode, and static SDL3 / SDL3_ttf libraries on macOS.

[Back to the project README](../README.md) · [iOS CI workflow](../.github/workflows/ios.yml)

## Project structure

| File | Purpose |
| --- | --- |
| `Info.plist.in` | iOS bundle metadata: fullscreen landscape, ProMotion (`CADisableMinimumFrameDurationOnPhone`), `UIFileSharingEnabled` |
| `build-ios.sh` | Configures & builds `OpenArtOfWar.app` for `device` (`iphoneos`) or `simulator` (`iphonesimulator`), with optional `--ipa` packaging |
| `../cmake/IosDeps.cmake` | Fetches and builds static dependencies from source (`SDL3`, `SDL3_ttf`, `yaml-cpp`, `nlohmann_json`, `zstd`, `libarchive`) |
| `../CMakeLists.txt` (`if(IOS)`) | Compiles `openaow-core` statically (`OAOW_TOUCH=1`), embeds Metal `.msl` shaders when `spirv-cross` is installed, and bundles `ios-setup/assets/` |

## Prerequisites (macOS)

- macOS with Xcode 15+ and iOS SDK (iOS 15.0+ deployment target).
- CMake **3.22+**, Git, and `spirv-cross` (for translating precompiled SPIR-V shaders into embedded Metal `.msl` shaders):

```bash
brew install cmake ninja spirv-cross
```

The first configure downloads and compiles dependencies via `cmake/IosDeps.cmake`.

## Quick build (device or simulator)

```bash
# Unsigned device .app + .ipa
./ios-setup/build-ios.sh device --ipa

# Simulator build
./ios-setup/build-ios.sh simulator

# Device build using your local Xcode signing team
./ios-setup/build-ios.sh device --sign
```

To inspect/sign/deploy from the Xcode IDE instead, open `ios-setup/build-device/OpenArtOfWar.xcodeproj` after running configure, pick your Development Team in **Signing & Capabilities**, and press **Run**.

## Game assets (`GAME_DATA_URLS`)

On first launch, `Assets::InstallBundledData()` (`src/core/AssetStore.cpp`) copies any `*.blob` and `*.mod` listed in `bundled_files.txt` from the `.app` bundle into the app's writable storage (`SDL_GetPrefPath("badcast", "openaow")`).

Two ways to provide `assets.blob`:

- **CI / automated download (`GAME_DATA_URLS`):** uses the exact same `GAME_DATA_URLS` / `GAME_DATA_TOKEN` repository variables as Android ([ci-game-data.md](../docs/ci-game-data.md)). `tools/fetch_game_data.sh` downloads into `ios-setup/assets/`, which CMake bundles into `OpenArtOfWar.app`.
- **Manual sideload via Finder / Files:** `UIFileSharingEnabled` is enabled in `Info.plist.in`, so you can drag `assets.blob` (and `*.mod` into `mods/`) into **Open Art of War** via macOS Finder File Sharing or the iOS Files app without rebuilding.

## Notes on code signing & distribution

- CI builds an **unsigned** `openaow-ios-unsigned.ipa` (`CODE_SIGNING_ALLOWED=NO`) so pull requests and builds succeed without Apple certificates in repo secrets.
- Sideloading on a physical iPhone/iPad requires signing with a free or paid Apple Developer account (Xcode **Signing & Capabilities**, AltStore / SideStore, or CI Provisioning Profile secrets).
