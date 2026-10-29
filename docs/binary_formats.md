# Art of War 2: Liberation of Peru - Binary Formats Specification

This document details the reverse-engineered binary container structures, data encoding schemes, and fixed-point math conventions used by the Gear Games J2ME engine.

---

## 1. Sprite Sheet Container (`/i0`)
The file `/i0` (282,547 bytes) stores 42 packed RGBA texture sheets for units, buildings, explosions, projectiles, and UI icons.

### Container Structure
- **Stream Format**: Sequence of variable-length chunks terminated by byte `0xFF`.
- **Chunk Header**:
  - `byte[0]`: High byte of chunk length (`b0 & 0xFF`). If `b0 == 0xFF`, end of container.
  - `byte[1]`: Middle byte (`b1 & 0xFF`).
  - `byte[2]`: Low byte (`b2 & 0xFF`).
  - `Chunk Length = (b0 << 16) | (b1 << 8) | b2` (24-bit big-endian unsigned integer).
- **Chunk Payload**: Raw PNG image data of `Chunk Length` bytes.

### Alpha Mask Reconstruction
Each texture sheet `page` (0 to 41) consists of a pair of adjacent chunks:
- **Chunk `2 * page` (Base Layer)**: 8-bit indexed or RGB PNG image containing the diffuse sprite graphics.
- **Chunk `2 * page + 1` (Alpha Mask)**: 8-bit grayscale PNG image of the same dimensions.
  - If `length == 0`: Base image is already opaque or self-contained.
  - If `length > 0`: The low byte (blue / grayscale luminance) defines the 8-bit Alpha channel (`0` = transparent, `255` = opaque).
  - Target RGBA pixel: `ARGB = (Alpha << 24) | (RGB & 0x00FFFFFF)`.

---

## 2. Sprite Atlas, Composite Graphics & Structure Descriptor (`/d0`)
The file `/d0` (103,215 bytes) stores the complete graphics layout: 2,178 raw sprite atlas rectangles, 2,221 composite sprite hierarchies, 293 animation sequences, 146 building templates, and 405 unit layered body parts.

### Binary Layout
- **26 Byte Arrays (`f220i[0..25]`)**:
  - Each array prefixed with a 16-bit big-endian signed short `length`.
  - `Array 0` (len 2178): `pageId` (texture sheet index 0..41). Page 8 = Blue Resistance, Page 9 = Red Confederation.
  - `Array 1` (len 2178): `srcX` (texture sub-region X, 0..255).
  - `Array 2` (len 2178): `srcY` (texture sub-region Y, 0..255).
  - `Array 3` (len 2178): `width` (frame width in pixels).
  - `Array 4` (len 2178): `height` (frame height in pixels).
  - `Array 5` (len 2178): `paletteId` (geometric color LUT index in `c.f569a`).
  - `Array 6` (len 6940): `isComposite` flag for composite sprite parts (`0` = raw sprite `e`, `1` = child composite `m29a`).
  - `Arrays 7 & 8` (len 1543): Animation frame offsets X and Y (signed 8-bit).
  - `Array 9` (len 1543): Animation frame composite flag (`0` = raw sprite, `1` = composite).
  - `Arrays 10 & 11` (len 992): Unit layer offsets X and Y (signed 8-bit).
  - `Array 12` (len 992): Unit layer composite flag.
  - `Array 13` (len 992): Unit layer type flag (`1` = shadow/tracks, `4` = rotating turret, `0` = body/hull).
  - `Arrays 15 & 16` (len 797): Unit sequence step offsets X and Y (signed 8-bit).
  - `Arrays 18 & 19` (len 146): Building footprint dimensions in tiles (`width` and `height`).
  - `Array 20` (len 996): Building part offset Y (signed 8-bit).
  - `Array 23` (len 996): Building part composite flag (`0` = raw sprite, `1` = composite, `2` = animation).
  - `Array 24` (len 996): Building layer index (`0` = ground foundation/shadow, `1` = main structure, `2` = rooftop/chimney).
  - `Array 25` (len 996): Building part state/progress filter (`0` = completed active structure, `5..7` = construction stages/scaffolding, `10` = destroyed ruin). Filtering by state prevents unfinished scaffolding or ruins from overlapping finished buildings.
- **18 Short Arrays (`f221f[0..17]`)**:
  - Each array prefixed with a 16-bit big-endian signed short `count`.
  - `Short Arrays 0 & 1` (len 2221): Composite sprite range `[start, end]` in parts table.
  - `Short Array 2` (len 6940): Composite part child sprite/composite ID.
  - `Short Arrays 3 & 4` (len 6940): Composite part offsets X and Y (signed 16-bit).
  - `Short Arrays 5 & 6` (len 293): Animation sequence range `[start, end]` in frames table.
  - `Short Array 7` (len 1543): Animation frame child sprite/composite ID.
  - `Short Arrays 8 & 9` (len 405): Unit body range `[start, end]` in layers table.
  - `Short Array 10` (len 992): Unit layer child sprite/composite ID.
  - `Short Arrays 11 & 12` (len 263): Unit action sequence range `[start, end]` in steps table.
  - `Short Array 13` (len 797): Unit sequence step body ID.
  - `Short Arrays 14 & 15` (len 146): Building template range `[start, end]` in building parts table.
  - `Short Array 16` (len 996): Building part child sprite/composite ID.
  - `Short Array 17` (len 996): Building part offset X (signed 16-bit).

---

## 2.5 Tileset & Terrain Mapper (`/ml`)
The `/ml` binary defines the mapping from 8-bit map tile indices and decor IDs to composite sprites:
1. `u8 tilesetCount`: Number of tilesets (2 tilesets in base game: Grassland/Jungle and Desert/Wasteland).
2. For each tileset:
   - `u16 tileCount`: Number of tile entries (e.g. 342 or 340).
   - Array of `tileCount` 16-bit big-endian signed shorts `compositeIds`: Maps `tileIndex` directly to composite sprite ID.
   - Array of 30 16-bit big-endian signed shorts `metadata`:
     - `metadata[14]`: Decor offset index in `compositeIds` array (e.g. 245). Decor composite ID is `compositeIds[metadata[14] + decorType]`.
   - Tile dimensions on screen: **Width = 30 pixels, Height = 20 pixels**.

---

## 3. Map Binary Container (`m0`, `m26`, `m35`, `m37`, `m42`, `m43`, `m44`)
Maps are binary files loaded into the 128x128 global grid.

### File Format:
1. `u8 width`: Map width in tiles (e.g. 60, 80, 100).
2. `u8 height`: Map height in tiles (e.g. 40, 60, 80, 100).
3. `u8 tilesetId`: Biome/tileset index (0 or 1, mapped via `/ml`).
4. `u16 rleSpanCount`: Number of Run-Length Encoded terrain spans (big-endian).
   - For each span:
     - `u8 runLength`: Number of consecutive tiles.
     - `u8 tileType`: Terrain tile index (0..341, mapped via `/ml` to composite ID).
5. `u16 decorCount`: Number of static objects/decorations (trees, rocks, ruins).
   - For each decor:
     - `u8 type`: Decoration archetype ID (mapped via `/ml` at `metadata[14] + type` to composite ID).
     - `u8 x`: Tile X anchor.
     - `u8 y`: Tile Y anchor.
6. `Passability Bitmasks`:
   - Array of `height` rows.
   - Each row has `ceil(width / 32)` 32-bit big-endian integers (`u32`).
   - Bit `1` at tile `(x, y)` marks blocked/impassable terrain; bit `0` marks passable terrain.

---

## 4. Mission Scripting & Spawn Container (`mi0` .. `mi7`)
Controls campaign mission progression, objectives, initial unit placement, and reinforcement waves.

### File Format:
1. `u8 mapIndex`: Index of map binary to load (e.g. `37` -> `m37`).
2. `u8 entityCount`: Number of initial units/structures to spawn.
   - For each entity:
     - `u8 unitTypeId`: Unit/building type ID (0..14 = units, 15+ = buildings).
     - `u8 tileX`: Initial tile X coordinate.
     - `u8 tileY`: Initial tile Y coordinate.
     - `u8 direction`: Initial facing orientation (0..15).
     - `faction`: Automatically assigned: `unitTypeId / 8` (`0` = Player Resistance, `1` = Confederation).
3. `Wave triggers and scripting opcodes`: Remaining payload defining AI waves, attack thresholds, and win/loss conditions.

---

## 5. Audio MIDI Container (`/s0`)
The file `/s0` (26,684 bytes) contains three concatenated standard Type 1 MIDI sequences:
- **Track 0** (Offset `0`, Length `10,550` bytes): Main Menu & Mission Briefing Theme (`music_track_0.mid`).
- **Track 1** (Offset `10,550`, Length `7,710` bytes): Battle Combat Theme 1 (`music_track_1.mid`).
- **Track 2** (Offset `18,260`, Length `8,424` bytes): Battle Combat Theme 2 (`music_track_2.mid`).

---

## 6. Game Balance & Unit Rules (`/a` and `/d`)
The file `/a` (6,108 bytes) and `/d` (226 bytes) store balance tables, unit health, speeds, ranges, costs, build times, and damage matrices.
- Stored as **160 Little-Endian 16-bit byte blocks**:
  - `u16 length` (little-endian: `low | (high << 8)`)
  - `length` bytes of attribute data.
- Followed by **9 Short Tables** in `/a` (damage vs armor multiplier matrices, research dependencies).

---

## 7. Fixed-Point Mathematics & Units
Due to CLDC-1.0 lacking hardware floating-point operations, all spatial calculations use fixed-point integers:
- **Tile Dimensions**: `30 x 20` pixels (`TILE_WIDTH = 30`, `TILE_HEIGHT = 20`).
- **Sub-tile Fine Position**: Values `0..29` (X) and `0..19` (Y) stored in sub-tile coordinates.
- **World Position**: `worldPixelX = tileX * 30 + subTileX`, `worldPixelY = tileY * 20 + subTileY`.
- **Direction / Rotation**: 16 discrete angles (`0..15`, step = 22.5 degrees), indexed into trigonometric lookup tables.
- **Velocity & Speeds**: 8-bit fractional fixed-point (`Q8.8` or `Q16.8`) where speed `1.0` tile/tick = `256`.
- **Simulation Timestep**: 50 ms tick interval (20 TPS fixed timestep).
