# Open Art Of War Map Editor

A standalone SDL3 and Dear ImGui application for editing Open Art Of War maps. It uses the game's shared renderer and asset pipeline and saves maps in the JSON format consumed by the game.

## Build

Build from the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target openaow-mapeditor -j$(nproc)
```

The project requires SDL3, SDL3_ttf, yaml-cpp, nlohmann-json, libarchive, and a C++17 toolchain. When `msgfmt` is installed, CMake also compiles the gettext catalogs.

## Run

From the repository root, open the default map or pass a map JSON path:

```bash
./build/openaow-mapeditor
./build/openaow-mapeditor assets/maps/m0.json
```

To run the headless editor self-test:

```bash
./build/openaow-mapeditor --test
```

## Editing

The editor supports painting terrain, flood filling, placing decorations, walls, buildings, and units, editing passability, and setting player and enemy starting positions. It includes undo/redo and a map generator (`MapGenerator.cpp`) modelled on the original maps: "Generate map" makes one in a click, `Ctrl+G` opens it with its settings. The terrain tiles form a corner set (each corner is light grass, dark grass, earth, sand or water; `include/TileCorners.hpp`, written by `tools/derive_tile_corners.py` from the tile pictures and the original maps), so the generator lays the ground out on the tile corners and never leaves a seam. Everything impassable is an island of overlapping rock and tree decorations, as in the originals; the open ground stays free, and the bases are joined by open roads. Every share (islands, closed map edge, rocks, decoration overlap, dark grass, earth/sand, speckled tiles, rubble, water and jungle under the islands) is a percentage slider in the generator dialog. The defaults, and the "Values of the original maps" button, give the medians measured on the original maps of the chosen tileset, and each slider's tooltip shows the range seen there. `openaow-mapeditor --generate-map out.json [seed] [tileset]` writes a map with those defaults without opening the window.

Passability follows the tile sprite, as in the original maps: bare earth and jungle tiles block, grass, grass/earth transition and rubble tiles do not. The table lives in `assets/data/maps_tile_collision.json` and is derived from the collision masks of the `map_mission_*.json` / `map_skirmish_*.json` maps by `tools/derive_tile_collision.py` (strict yes/no for every tile; tiles of one variation range share the answer). The game's map loader (`TileMap::loadMap`) keeps a blocking tile blocked whatever the file's mask says, and painting, filling, the generators and the passability recalculation of the editor use the same table (`TileCollision`); blocking tiles carry a red corner in the tile palette. Without the file the old rule (tiles from 211 / 191 up block) applies. Use the ImGui panels to choose the active tool and its palette or settings. Save the finished map as JSON and pass that file to the game or reopen it in the editor.

## Controls

| Action | Control |
| --- | --- |
| Paint or use the active tool | Left mouse button |
| Move player start marker | Left mouse button drag on start marker (P1–P8) |
| Context menu (set/add player starts) | Right mouse button click on tile |
| Quick erase brush | Right mouse button drag |
| Pan the camera | Middle mouse button and drag |
| Zoom | Mouse wheel |
| Move the camera | `W`, `A`, `S`, `D` or arrow keys |
| Center the camera on the map | `F` |
| New map | `Ctrl+N` |
| Open map | `Ctrl+O` |
| Save map | `Ctrl+S` |
| Undo / redo | `Ctrl+Z` / `Ctrl+Y` |
| Open the map generator | `Ctrl+G` |
| Toggle grid | `G` |
| Toggle passability overlay | `C` |
| Toggle minimap | `M` |

## Map Format

Maps are stored as JSON. Maps live in `assets/maps/`, the same place the game reads them from; the editor finds `assets` next to the executable (or one level up), like the game does, so it can be started from any directory.

When the game is installed with only `data.blob` (no `assets/` directory), the editor reads the packed maps and saves new ones to the per-user data directory (`~/.local/share/openaow/openaow/maps/`), which the game also reads first.

The optional `"title"` key of a map file is the English name of the map and doubles as the gettext msgid: the game shows `_(title)`, so a translation goes into `po/openaow_<lang>.po` (see `tools/localegen.sh`) and the map file stays language-neutral. A title without a translation is shown as it is.
