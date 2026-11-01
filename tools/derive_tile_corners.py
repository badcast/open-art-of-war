#!/usr/bin/env python3
"""Derive the ground material in the four corners of every terrain tile and write the map editor's table.

The terrain tiles of both tilesets form a corner (Wang) set: each corner of a tile is one ground material
(G light grass, D dark grass, E earth, W water and, in tileset 1, S sand), and neighbouring tiles share
their corners. The material of each corner is first guessed from the colour of the tile picture (the corner
patch is compared with the mean colour of the plain tiles of every material), then settled by a vote of
the neighbouring corners in the original maps (map_mission_*.json, map_skirmish_*.json), which keeps the
picture's guess as a prior. Rubble, jungle and cliff tiles (211+ / 191+) are not ground and stay out.

Writes openaow-mapeditor/include/TileCorners.hpp, used by the Smart map generator.

Usage: tools/derive_tile_corners.py [repo root]   (needs Pillow)
"""
import collections
import glob
import json
import os
import sys

from PIL import Image

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..'))
ASSETS = os.path.join(ROOT, 'assets')
OUT = os.path.join(ROOT, 'openaow-mapeditor', 'include', 'TileCorners.hpp')

GROUND_TILES = 211  # tiles 0..210 are ground; above come rubble, jungle and cliffs
# Plain tiles of every material: the colour references
PURE = {0: {'G': (0, 9), 'D': (10, 17), 'E': (19, 25), 'W': (26, 33)},
        1: {'G': (0, 9), 'D': (10, 17), 'S': (18, 25), 'E': (26, 32), 'W': (33, 35)}}
# Sheet widths of the original build (TextureManager::BaseSheetWidth): the PNG sheets may be upscaled
BASE_SHEET_WIDTH = [240, 179, 206, 229, 200, 274, 268, 240, 77, 77, 107, 153, 91, 112, 223, 154, 193, 158, 135, 73,
                    65, 121, 91, 94, 106, 169, 68, 78, 126, 231, 272, 45, 166, 123, 122, 1, 61, 88, 50, 63, 98, 33]

graphics = json.load(open(os.path.join(ASSETS, 'data', 'graphics_data.json')))
sprites = {s['id']: s for s in json.load(open(os.path.join(ASSETS, 'characters', 'sprites.json')))}
sheets = {}


def sprite(sprite_id):
    frame = sprites.get(sprite_id)
    if not frame or frame['width'] <= 0 or frame['height'] <= 0:
        return None
    page = frame['page']
    if page not in sheets:
        sheets[page] = Image.open(os.path.join(ASSETS, 'sprites', f'sheet_{page:02d}.png')).convert('RGBA')
    sheet = sheets[page]
    scale = sheet.width / BASE_SHEET_WIDTH[page]
    box = tuple(round(v * scale) for v in (frame['x'], frame['y'], frame['x'] + frame['width'], frame['y'] + frame['height']))
    return sheet.crop(box).resize((frame['width'], frame['height']), Image.LANCZOS)


def draw_composite(canvas, comp_id, x, y, depth=0):
    ranges, parts = graphics['composite_sprites']['ranges'], graphics['composite_sprites']['parts']
    if depth > 6 or not 0 <= comp_id < len(ranges):
        return
    first, last = ranges[comp_id]
    for part in parts[first:last + 1]:
        px, py = x + part['ox'], y + part['oy']
        if part['is_comp']:
            draw_composite(canvas, part['child'], px, py, depth + 1)
        elif (image := sprite(part['child'])) is not None:
            canvas.alpha_composite(image, (int(px), int(py)))


def tile_image(tileset, tile):
    canvas = Image.new('RGBA', (30, 20), (0, 0, 0, 255))
    draw_composite(canvas, graphics['tilesets'][tileset][tile], 0, 0)
    return canvas.convert('RGB')


def mean_colour(image, box):
    pixels = list(image.crop(box).getdata())
    return tuple(sum(p[i] for p in pixels) / len(pixels) for i in range(3))


CORNER_BOXES = [(0, 0, 7, 5), (23, 0, 30, 5), (0, 15, 7, 20), (23, 15, 30, 20)]  # NW NE SW SE


def guess_from_pictures(tileset):
    refs = {}
    for material, (first, last) in PURE[tileset].items():
        colours = [mean_colour(tile_image(tileset, t), (0, 0, 30, 20)) for t in range(first, last + 1)]
        refs[material] = tuple(sum(c[i] for c in colours) / len(colours) for i in range(3))
    table = {}
    for tile in range(GROUND_TILES):
        image = tile_image(tileset, tile)
        code = ''
        for box in CORNER_BOXES:
            colour = mean_colour(image, box)
            code += min(refs, key=lambda m: sum((colour[i] - refs[m][i]) ** 2 for i in range(3)))
        table[tile] = code
    return table


def load_map(path):
    data = json.load(open(path))
    width, height = data['width'], data['height']
    tiles = []
    for run in data['terrainRle']:
        tiles += [run['tileType']] * run['runLength']
    return data['tilesetId'], width, height, (tiles + [0] * (width * height))[:width * height]


# Neighbour offset -> pairs (corner of this tile, corner of the neighbour) that touch
RELATIONS = [((1, 0), [(1, 0), (3, 2)]), ((0, 1), [(2, 0), (3, 1)]), ((1, 1), [(3, 0)]), ((-1, 1), [(2, 1)])]


def is_pure_base(tileset, tile):
    return any(first <= tile <= last for first, last in PURE[tileset].values())


def vote(tables, maps, rounds=6):
    for _ in range(rounds):
        votes = {ts: collections.defaultdict(lambda: [collections.Counter() for _ in range(4)]) for ts in tables}
        for tileset, width, height, tiles in maps:
            table = tables[tileset]
            for y in range(height):
                for x in range(width):
                    tile = tiles[y * width + x]
                    if tile not in table:
                        continue
                    for (dx, dy), pairs in RELATIONS:
                        for sign in (1, -1):
                            nx, ny = x + dx * sign, y + dy * sign
                            if not (0 <= nx < width and 0 <= ny < height):
                                continue
                            other = table.get(tiles[ny * width + nx])
                            if not other:
                                continue
                            for mine, theirs in pairs:
                                if sign == 1:
                                    votes[tileset][tile][mine][other[theirs]] += 1
                                else:
                                    votes[tileset][tile][theirs][other[mine]] += 1
        changed = 0
        for tileset, table in tables.items():
            for tile, code in list(table.items()):
                if is_pure_base(tileset, tile) or tile not in votes[tileset]:
                    continue
                counters = votes[tileset][tile]
                new_code = ''
                for corner in range(4):
                    counters[corner][code[corner]] += 3  # the picture's guess as a prior
                    new_code += counters[corner].most_common(1)[0][0]
                if new_code != code:
                    table[tile] = new_code
                    changed += 1
        if not changed:
            break
    return tables


def main():
    paths = sorted(glob.glob(os.path.join(ASSETS, 'maps', 'map_mission_*.json')) +
                   glob.glob(os.path.join(ASSETS, 'maps', 'map_skirmish_*.json')))
    maps = [load_map(p) for p in paths]
    tables = vote({ts: guess_from_pictures(ts) for ts in (0, 1)}, maps)

    lines = ['#pragma once', '',
             '// Generated by tools/derive_tile_corners.py from the tile pictures and the original maps; do not edit.',
             '// Ground material in the corners of terrain tiles 0..210, four letters in the order NW NE SW SE:',
             '// G light grass, D dark grass, E earth, W water, S sand (tileset 1 only).', '',
             'namespace OpenArtOfWar::TileCorners {', '',
             f'constexpr int kGroundTiles = {GROUND_TILES};', '']
    for tileset in (0, 1):
        lines.append(f'constexpr const char* kTileset{tileset}[kGroundTiles] = {{')
        codes = [f'"{tables[tileset][t]}"' for t in range(GROUND_TILES)]
        for i in range(0, len(codes), 12):
            lines.append('    ' + ', '.join(codes[i:i + 12]) + ',')
        lines += ['};', '']
    lines += ['} // namespace OpenArtOfWar::TileCorners', '']
    with open(OUT, 'w') as out:
        out.write('\n'.join(lines))
    print(f'Wrote {os.path.relpath(OUT, ROOT)} from {len(paths)} maps')


if __name__ == '__main__':
    main()
