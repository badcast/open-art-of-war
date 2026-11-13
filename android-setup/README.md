# Open Art of War — Android Build Setup

This directory contains the Android project for **Open Art of War**, built with Gradle, the Android NDK, CMake and SDL3.

[Back to the project README](../README.md) · [Android CI workflow](../.github/workflows/android.yml)

## Project structure

| File | Purpose |
| --- | --- |
| `build.gradle.kts`, `settings.gradle.kts` | Root Gradle configuration |
| `app/build.gradle.kts` | Application configuration: SDK 34, Android 7.0+ (API 24), ARM64 and x86_64, C++17 and CMake |
| `app/src/main/AndroidManifest.xml` | App permissions, launcher icon, landscape orientation and SDLActivity entry point |
| `app/src/main/cpp/CMakeLists.txt` | Builds the game through the repository's root CMake configuration; disables desktop editors, server and tests |
| `fetch-sdl-java.sh` | Downloads the SDL Java sources matching the native SDL version |

Native dependencies are built from source through [`cmake/AndroidDeps.cmake`](../cmake/AndroidDeps.cmake). SDLActivity starts the game's `libmain.so`, which links the engine and Android system libraries.

## Prerequisites

- Android Studio or the Android SDK command-line tools.
- Android SDK Platform 34 and Build Tools 34.0.0.
- Android NDK; CI uses version **26.1.10909125**.
- CMake **3.22.1+**.
- JDK **17** and Gradle **8.7**, matching the CI configuration.
- Git and a POSIX shell to run the SDL Java source download script.

The first build requires internet access to download dependencies.

## Prepare SDL Java sources

Run this once from the repository root before building:

```bash
./android-setup/fetch-sdl-java.sh
```

Run it again when the project's SDL version changes. On Windows, use Git Bash or another POSIX shell.

## Build with Android Studio

1. Prepare the SDL Java sources as described above.
2. Open the `android-setup` directory using **File → Open**.
3. Wait for Gradle Sync and dependency downloads to finish.
4. Build the debug APK or run the app on a connected device or emulator.

## Build from the command line

With `ANDROID_HOME` pointing to the Android SDK and Gradle available on your `PATH`, run:

```bash
cd android-setup
gradle assembleDebug
```

The resulting APK is located at this path relative to the repository root:

```text
android-setup/app/build/outputs/apk/debug/app-debug.apk
```

To install it on a connected device, run from the repository root:

```bash
adb install -r android-setup/app/build/outputs/apk/debug/app-debug.apk
```

## Game assets

The game reads `assets.blob` by filesystem path (libarchive cannot read inside an APK). Two ways to get it onto the device:

- **CI / your own build:** list the files in the repository variable `GAME_DATA_URLS`; they are bundled into the APK and
  copied to the app's storage on the first start ([instructions](../docs/ci-game-data.md)). Locally: run
  `GAME_DATA_URLS=... tools/fetch_game_data.sh game-data`, copy `game-data/*` into `android-setup/app/src/main/assets/`.
- **By hand:** push the archive as below.

After installing and launching the app once, copy your prepared archive into the app's storage directory. Run this command from the repository root:

```bash
adb push build/assets.blob /sdcard/Android/data/org.openartofwar/files/assets.blob
```

Restart the app after copying the archive. See the [main asset instructions](../README.md#game-assets) for asset requirements and how to pack an existing `assets/` directory.

## Android interface and controls

CMake defines `OAOW_TOUCH=1` for mobile builds (Android and iOS) and `OAOW_TOUCH=0` for desktop builds. Android-specific calls are isolated in [`platform/TouchPlatform.cpp`](../platform/TouchPlatform.cpp).

- Fullscreen landscape mode supports both landscape orientations.
- The interface uses a shared coordinate system with a height of 720, preserving the screen's aspect ratio.
- HUD tabs use two rows of labelled, enlarged buttons.
- The launcher icon comes from `docs/app-game-icon.png` and is included as `app/src/main/res/mipmap-nodpi/ic_launcher.png`.

| Action | Gesture |
| --- | --- |
| Select a unit, place a building or activate a button | Tap |
| Select multiple units | Drag with one finger on the map |
| Move, attack or set a rally point | Hold for at least 0.5 seconds, then release |
| Cancel a production order | Hold its production card, then release |
| Move and zoom the camera | Drag or pinch with two fingers |
| Switch HUD tabs | Tap the labelled sidebar buttons |
| Back / game menu | System Back button, mapped to Escape |

Camera gestures are disabled while menus are open.

## Validation

Run the touch input tests in a configured desktop build, without an Android device:

```bash
cmake --build build --target openaow-android-input-tests
ctest --test-dir build -R android-input --output-on-failure
```

On a device, also check the HUD, menus, research window, on-screen keyboard, screen rotation, and background/resume behaviour.
