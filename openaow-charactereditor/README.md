# Character Archetype Editor

A standalone SDL3 and Dear ImGui tool for inspecting and editing the YAML character and building definitions in `assets/characters/`. The preview uses the game's `TextureManager` and `Renderer` with the original sprite sheets and graphics tables.

## Build

Build from the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target openaow-charactereditor -j$(nproc)
```

The target uses the project's SDL3, SDL3_ttf, yaml-cpp, nlohmann-json, libarchive, and Dear ImGui dependencies. When `msgfmt` is installed, the build also compiles the gettext catalogs.

## Run

Run from the repository root so the editor can find the game assets:

```bash
./build/openaow-charactereditor
```

The editor opens in English by default. Use **Language** in the menu bar to switch between English and Russian.

## Workflow

- Select a unit or building from the searchable archetype list on the left.
- Edit identity, size, stats, production, combat, animation metadata, and abilities in the properties panel.
- Preview the game's idle, move, attack, and death states. Adjust facing, playback speed, zoom, and health from 0% to 100%; at 0%, units play their death animation and vehicles/buildings show their wreck or rubble. For buildings, preview construction progress and power state.
- Use **Save** or `Ctrl+S` to update the selected YAML file. Use **Save As** to create a YAML file in `assets/characters/`, or **Reload file** to discard unsaved edits and reload from disk.

The editor preserves fields not exposed in its controls when saving. The preview uses the game's graphics tables; animation metadata fields that the engine does not currently consume remain configuration metadata. See [`assets/characters/README.md`](../assets/characters/README.md) for the YAML field reference.
