# Game data in the CI builds (assets.blob and mods)

`assets/` is not in this repository, so a CI build has no game data of its own. The workflows can fetch it: the files
you list (an `assets.blob` and any number of `*.mod`) are downloaded during the build and put

- into the **Android APK** (`android-setup/app/src/main/assets/`): the game copies them to its storage on the first
  start (`*.blob` next to where blobs are looked for, `*.mod` into `mods/`); an update of the APK with a file of
  another size replaces the copy;
- into the **iOS app bundle** (`ios-setup/assets/`, bundled into `OpenArtOfWar.app`): `Assets::InstallBundledData()`
  copies `*.blob` and `*.mod` listed in `bundled_files.txt` to `SDL_GetPrefPath("badcast", "openaow")` on the first
  start in the same way;
- into the **desktop packages** (Linux, Windows MSVC / MinGW, macOS): `assets.blob` next to the executable, mods in
  `mods/`.

## Setting it up in the GitHub interface

1. Put the files somewhere the runner can download them: a GitHub Release of this or another repository, or any
   https address. Pack the assets with `tools/pack_assets.py assets assets.blob` (the CMake target `pack-assets` does
   the same); a mod is a tar compressed with zstd with `mod.yaml` at its root (`docs/modding.md`).
2. Repository **Settings -> Secrets and variables -> Actions -> Variables -> New repository variable**:
   - name `GAME_DATA_URLS`
   - value: one entry per line
     ```
     https://github.com/OWNER/REPO/releases/download/data-1/assets.blob
     https://github.com/OWNER/REPO/releases/download/data-1/better_tanks.mod
     renamed.mod = https://example.com/download?id=7
     # a line starting with # is ignored
     ```
     A name must end in `.blob` or `.mod`; `name = url` gives the file a name when the address does not end in one.
3. Private files (a release of a private repository, a URL that must not be public): use **Secrets** instead of a
   variable. A secret `GAME_DATA_URLS` works the same way (the addresses stay out of the logs), and a secret
   `GAME_DATA_TOKEN` (a personal access token that may read the repository) is sent as `Authorization: Bearer`. For a
   private release use the asset's API address `https://api.github.com/repos/OWNER/REPO/releases/assets/ID`
   (the script asks for the binary).
4. Run the workflow: push to `main`, or **Actions -> Android Build (or Build / iOS Build) -> Run workflow**; the dialog has a
   `data_urls` field that overrides the variable for that one run.

Order of precedence: the `data_urls` field of a manual run, then the variable `GAME_DATA_URLS`, then the secret
`GAME_DATA_URLS`. With nothing set the build has no game data (it still builds; push `assets.blob` by hand as before,
[`android-setup/README.md`](../android-setup/README.md) or [`ios-setup/README.md`](../ios-setup/README.md)).

## What the workflow does

`tools/fetch_game_data.sh [dir]` reads `GAME_DATA_URLS` / `GAME_DATA_TOKEN`, downloads every entry (3 retries), refuses
names that are not plain `*.blob` / `*.mod`, and writes `bundled_files.txt` (the names) next to them. A failed or empty
download fails the step. Locally:

```bash
GAME_DATA_URLS=$'file:///path/to/assets.blob\nfile:///path/to/my.mod' tools/fetch_game_data.sh game-data
```

Android: the files and `bundled_files.txt` are copied to `android-setup/app/src/main/assets/`; `androidResources.noCompress`
keeps the archives stored as they are. Installed files are logged as `[Assets] installed bundled ...`
(`adb logcat | grep Assets`). The blob must be one the game accepts (it carries the certificate written by
`pack_assets.py`); the APK gets bigger by the size of the data.

iOS: the files and `bundled_files.txt` are downloaded to `ios-setup/assets/`, which `CMakeLists.txt` (`if(IOS)`) bundles
into `OpenArtOfWar.app` as bundle resources.

Desktop: `Package` steps copy `game-data/*.blob` to the package folder and `game-data/*.mod` to its `mods/`.
