#!/usr/bin/env python3
"""
Art of War 2 - unpack the game data of an original build into assets/.

One tool for everything that turns the original files into game assets:
  audio       MIDI tracks of the soundtrack (and OGG renders when assets/audio/soundfont.sf2 exists)
  sprites     sprite sheets (RGBA, alpha masks merged)
  graphics    sprite rectangles (characters/sprites.json) and composites / animations / buildings (data/graphics_data.json)
  tilesets    ground tilesets and UI images
  strings     UI, help and campaign dialogue strings
  maps        m<N> campaign maps
  skirmish    map<N> maps of the community builds (3-byte header + map record), as map_skirmish_<N>.json
  missions    mission list and the scripts of mi<N>
  rules       palettes and the balance tables
  titles      in-game titles of the campaign maps (from the mission names in 0/s0)
  collision   (not in the default set) tile passability, derived from the map_*.json of the destination

--from takes the original build as a directory (already unpacked) or an archive (.jar / .im / .zip);
--dest is the assets directory to write to.

With --game-names the campaign maps are named like the game's own assets: map_mission_<N+1>.json
(m<N> otherwise).

Examples:
  unpackassets.py --from original_extracted --dest assets
  unpackassets.py --from AoW2.jar --dest /tmp/assets --only maps,titles
  unpackassets.py --from "NEWMAPS/Art_of_war_2_global_confederation-spaces.im" --dest assets --game-names
"""

import argparse
import io
import json
import os
import re
import struct
import sys
import tempfile
import zipfile
from PIL import Image

# Set from the command line in main()
SRC_DIR = "original_extracted"
OUT_DIR = "assets"
GAME_NAMES = False

def setup_dirs():
    subdirs = ["characters", "sprites", "tilesets", "maps", "missions", "audio", "ui", "localization", "data"]
    for s in subdirs:
        os.makedirs(os.path.join(OUT_DIR, s), exist_ok=True)

def extract_audio():
    print("Extracting MIDI Soundtrack...")
    s0_path = os.path.join(SRC_DIR, "s0")
    if not os.path.exists(s0_path):
        print("  Error: s0 not found")
        return

    with open(s0_path, "rb") as fh:
        data = fh.read()

    # Track slices according to EngineCore.f276e and f275d
    tracks = [
        {"name": "music_track_0.mid", "offset": 0, "length": 10550, "desc": "Main Menu & Briefing Theme"},
        {"name": "music_track_1.mid", "offset": 10550, "length": 7710, "desc": "Battle Combat Theme 1"},
        {"name": "music_track_2.mid", "offset": 18260, "length": 8424, "desc": "Battle Combat Theme 2"}
    ]

    for t in tracks:
        chunk = data[t["offset"]:t["offset"] + t["length"]]
        out_path = os.path.join(OUT_DIR, "audio", t["name"])
        with open(out_path, "wb") as out_fh:
            out_fh.write(chunk)
        print(f"  Extracted: {t['name']} ({len(chunk)} bytes) - {t['desc']}")

        # Render high quality OGG via FluidSynth if soundfont is present
        sf_path = os.path.join(OUT_DIR, "audio", "soundfont.sf2")
        ogg_path = out_path.replace(".mid", ".ogg")
        if os.path.exists(sf_path):
            import subprocess
            wav_tmp = f"/tmp/{t['name']}.wav"
            subprocess.run(["fluidsynth", "-ni", "-F", wav_tmp, "-r", "44100", sf_path, out_path],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(["ffmpeg", "-y", "-i", wav_tmp, "-c:a", "libvorbis", "-q:a", "7", ogg_path],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if os.path.exists(wav_tmp):
                os.remove(wav_tmp)
            print(f"  Synthesized Studio OGG: {ogg_path}")

def extract_sprites():
    print("\nExtracting Sprite Sheets and Merging Alpha Masks...")
    i0_path = os.path.join(SRC_DIR, "i0")
    if not os.path.exists(i0_path):
        print("  Error: i0 not found")
        return

    with open(i0_path, "rb") as fh:
        data = fh.read()

    offset = 0
    chunks = []
    while offset < len(data):
        b0 = data[offset]
        offset += 1
        if b0 == 255:
            break
        length = (b0 << 16) | (data[offset] << 8) | data[offset + 1]
        offset += 2
        chunk = data[offset:offset + length]
        offset += length
        chunks.append(chunk)

    print(f"  Loaded {len(chunks)} raw chunks from i0. Reconstructing 42 texture sheets...")

    # Chunks are paired: even = base image, odd = alpha mask
    sheet_count = len(chunks) // 2
    for page in range(sheet_count):
        base_chunk = chunks[page * 2]
        mask_chunk = chunks[page * 2 + 1]

        if len(base_chunk) == 0:
            continue

        base_img = Image.open(io.BytesIO(base_chunk)).convert("RGBA")
        if len(mask_chunk) > 0:
            mask_img = Image.open(io.BytesIO(mask_chunk)).convert("L")
            if mask_img.size == base_img.size:
                r, g, b, _ = base_img.split()
                combined = Image.merge("RGBA", (r, g, b, mask_img))
            else:
                combined = base_img
        else:
            combined = base_img

        out_name = f"sheet_{page:02d}.png"
        out_path = os.path.join(OUT_DIR, "sprites", out_name)
        combined.save(out_path)

    print(f"  Exported {sheet_count} RGBA sprite sheets to assets/sprites/")

def extract_sprite_definitions_and_d0():
    print("\nParsing /d0 (Sprite Rectangles, Animations, Sound FX Data)...")
    d0_path = os.path.join(SRC_DIR, "d0")
    if not os.path.exists(d0_path):
        print("  Error: d0 not found")
        return

    with open(d0_path, "rb") as fh:
        data = fh.read()

    offset = 0
    byte_arrays = []
    for _ in range(26):
        arr_len = struct.unpack(">h", data[offset:offset + 2])[0]
        offset += 2
        arr_data = data[offset:offset + arr_len]
        offset += arr_len
        byte_arrays.append(arr_data)

    short_arrays = []
    for _ in range(18):
        arr_len = struct.unpack(">h", data[offset:offset + 2])[0]
        offset += 2
        shorts = struct.unpack(f">{arr_len}h", data[offset:offset + arr_len * 2])
        offset += arr_len * 2
        short_arrays.append(shorts)

    # Sprite frames
    pages = byte_arrays[0]
    xs = byte_arrays[1]
    ys = byte_arrays[2]
    ws = byte_arrays[3]
    hs = byte_arrays[4]
    pals = byte_arrays[5]

    sprites = []
    for i in range(len(pages)):
        sprites.append({
            "id": i,
            "page": pages[i],
            "x": xs[i],
            "y": ys[i],
            "width": ws[i],
            "height": hs[i],
            "palette": pals[i]
        })

    with open(os.path.join(OUT_DIR, "characters", "sprites.json"), "w", encoding="utf-8") as out_fh:
        json.dump(sprites, out_fh, indent=2)
    print(f"  Exported {len(sprites)} sprite definitions to assets/characters/sprites.json")

    # Animation sequence tables
    def to_signed8(v):
        return v if v < 128 else v - 256

    # Load /ml for tilesets
    tilesets = []
    decor_offsets = []
    ml_path = os.path.join(SRC_DIR, "ml")
    if os.path.exists(ml_path):
        with open(ml_path, "rb") as mlfh:
            mldata = mlfh.read()
        mloffset = 1
        for _ in range(mldata[0]):
            tlen = struct.unpack(">h", mldata[mloffset:mloffset + 2])[0]
            mloffset += 2
            t = struct.unpack(f">{tlen}h", mldata[mloffset:mloffset + tlen * 2])
            mloffset += tlen * 2
            e_shorts = struct.unpack(">30h", mldata[mloffset:mloffset + 60])
            mloffset += 60
            tilesets.append(list(t))
            decor_offsets.append(e_shorts[14])

    graphics_data = {
        "composite_sprites": {
            "ranges": [[short_arrays[0][i], short_arrays[1][i]] for i in range(len(short_arrays[0]))],
            "parts": [
                {
                    "child": short_arrays[2][i],
                    "ox": short_arrays[3][i],
                    "oy": short_arrays[4][i],
                    "is_comp": int(byte_arrays[6][i])
                }
                for i in range(len(short_arrays[2]))
            ]
        },
        "anim_sequences": {
            "ranges": [[short_arrays[5][i], short_arrays[6][i]] for i in range(len(short_arrays[5]))],
            "frames": [
                {
                    "child": short_arrays[7][i],
                    "ox": to_signed8(byte_arrays[7][i]),
                    "oy": to_signed8(byte_arrays[8][i]),
                    "is_comp": int(byte_arrays[9][i])
                }
                for i in range(len(short_arrays[7]))
            ]
        },
        "unit_bodies": {
            "ranges": [[short_arrays[8][i], short_arrays[9][i]] for i in range(len(short_arrays[8]))],
            "layers": [
                {
                    "child": short_arrays[10][i],
                    "ox": to_signed8(byte_arrays[10][i]),
                    "oy": to_signed8(byte_arrays[11][i]),
                    "is_comp": int(byte_arrays[12][i]),
                    "flag": int(byte_arrays[13][i])
                }
                for i in range(len(short_arrays[10]))
            ]
        },
        "unit_sequences": {
            "ranges": [[short_arrays[11][i], short_arrays[12][i]] for i in range(len(short_arrays[11]))],
            "steps": [
                {
                    "body": short_arrays[13][i],
                    "ox": to_signed8(byte_arrays[15][i]),
                    "oy": to_signed8(byte_arrays[16][i])
                }
                for i in range(len(short_arrays[13]))
            ]
        },
        "buildings": {
            "ranges": [[short_arrays[14][i], short_arrays[15][i]] for i in range(len(short_arrays[14]))],
            "widths": [int(x) for x in byte_arrays[18]],
            "heights": [int(x) for x in byte_arrays[19]],
            "parts": [
                {
                    "child": short_arrays[16][i],
                    "ox": short_arrays[17][i],
                    "oy": to_signed8(byte_arrays[20][i]),
                    "is_comp": int(byte_arrays[23][i]),
                    "layer": int(byte_arrays[24][i]),
                    "state": int(byte_arrays[25][i])
                }
                for i in range(len(short_arrays[16]))
            ]
        },
        "tilesets": tilesets,
        "decor_offsets": decor_offsets,
        "unit_archetypes": [
            [142, 134, 231], [142, 134, 231], [166, 158, 231], [166, 158, 231],
            [206, 198, 231, 214], [190, 182, 231], [75, 67, 75], [8, 0, 8],
            [91, 83, 91], [24, 16, 24], [107, 99, 107, 115], [40, 32, 40],
            [126, 118, 126], [56, 48, 56, 64], [230, 230, 230, 222],
            [117, 117, 117, 116], [66, 66, 66, 65], [40, 32, 40], [8, 0, 8]
        ],
        "decor_anims": [1741, 1747, 1753, 1759, 1764, 1769, 1773, 1784, 1793, 1802, 1811]
    }

    with open(os.path.join(OUT_DIR, "data", "graphics_data.json"), "w", encoding="utf-8") as out_fh:
        json.dump(graphics_data, out_fh, indent=1)
    print(f"  Exported complete graphics_data.json ({len(short_arrays[0])} composites, {len(short_arrays[5])} anims, {len(short_arrays[14])} buildings, {len(tilesets)} tilesets)")

def extract_tilesets_and_ui():
    print("\nExtracting Tilesets and UI Images...")
    for i in range(6):
        src_name = f"r0{i}"
        src_path = os.path.join(SRC_DIR, src_name)
        if os.path.exists(src_path):
            img = Image.open(src_path)
            img.save(os.path.join(OUT_DIR, "tilesets", f"tileset_{i}.png"))
    print("  Tilesets 0..5 saved to assets/tilesets/")

    ui_mapping = {
        "l1": "logo.png",
        "l2": "title.png",
        "icon.png": "icon.png"
    }
    for src_name, dst_name in ui_mapping.items():
        src_path = os.path.join(SRC_DIR, src_name)
        if os.path.exists(src_path):
            img = Image.open(src_path)
            img.save(os.path.join(OUT_DIR, "ui", dst_name))
    print("  UI screens saved to assets/ui/")

def extract_strings():
    print("\nExtracting Localization & Campaign Dialogue Strings...")
    # 0/s0 -> UI strings
    s0_path = os.path.join(SRC_DIR, "0", "s0")
    if os.path.exists(s0_path):
        with open(s0_path, "rb") as fh:
            raw = fh.read()
        parts = [p.decode("windows-1251", errors="replace") for p in raw.split(b"|")]
        with open(os.path.join(OUT_DIR, "localization", "ui_strings.json"), "w", encoding="utf-8") as fh:
            json.dump(parts, fh, indent=2, ensure_ascii=False)
        print(f"  Extracted {len(parts)} UI strings from 0/s0")

    # 0/t0 -> Help & Tutorial strings
    t0_path = os.path.join(SRC_DIR, "0", "t0")
    if os.path.exists(t0_path):
        with open(t0_path, "rb") as fh:
            raw = fh.read()
        parts = [p.decode("windows-1251", errors="replace") for p in raw.split(b"|")]
        with open(os.path.join(OUT_DIR, "localization", "help_strings.json"), "w", encoding="utf-8") as fh:
            json.dump(parts, fh, indent=2, ensure_ascii=False)
        print(f"  Extracted {len(parts)} Help/Tutorial strings from 0/t0")

    # 0/d0 -> Campaign Story Briefings & Dialogues
    d0_path = os.path.join(SRC_DIR, "0", "d0")
    if os.path.exists(d0_path):
        with open(d0_path, "rb") as fh:
            raw = fh.read()
        parts = [p.decode("windows-1251", errors="replace") for p in raw.split(b"|")]
        with open(os.path.join(OUT_DIR, "localization", "campaign_dialogs.json"), "w", encoding="utf-8") as fh:
            json.dump(parts, fh, indent=2, ensure_ascii=False)
        print(f"  Extracted {len(parts)} Campaign dialogue entries from 0/d0")



def decode_map(data, off=0):
    """Map record: width, height, tileset, RLE terrain, decorations, collision bitmask. Returns (dict, end offset)."""
    pos = off

    def read_u(n):
        nonlocal pos
        val = int.from_bytes(data[pos:pos + n], "big")
        if pos + n > len(data):
            raise IndexError("map record is cut off")
        pos += n
        return val

    width, height, tileset_id = read_u(1), read_u(1), read_u(1)
    terrain = [{"runLength": read_u(1), "tileType": read_u(1)} for _ in range(read_u(2))]
    decors = [{"type": read_u(1), "x": read_u(1), "y": read_u(1)} for _ in range(read_u(2))]
    chunks = (width - 1) // 32 + 1
    collision = [[read_u(4) for _ in range(chunks)] for _ in range(height)]
    return {
        "width": width,
        "height": height,
        "tilesetId": tileset_id,
        "terrainRle": terrain,
        "decorations": decors,
        "collisionMask": collision,
    }, pos


def number_of(name):
    return int(re.search(r"\d+", name).group())


def mission_map_name(name):
    """File name (without .json) of the campaign map m<N>"""
    return f"map_mission_{number_of(name) + 1}" if GAME_NAMES else name


def extract_maps():
    print("\nDecoding campaign maps (m<N>)...")
    names = sorted((f for f in os.listdir(SRC_DIR) if re.fullmatch(r"m\d+", f)), key=number_of)
    for name in names:
        with open(os.path.join(SRC_DIR, name), "rb") as fh:
            data = fh.read()
        try:
            doc, _ = decode_map(data)
        except IndexError:
            print(f"  Warning: {name} is not a map record, skipped")
            continue
        out = mission_map_name(name)
        with open(os.path.join(OUT_DIR, "maps", f"{out}.json"), "w", encoding="utf-8") as fh:
            json.dump({"name": out, **doc}, fh, indent=2)
        print(f"  Decoded map {name} -> {out}.json: {doc['width']}x{doc['height']}, tileset={doc['tilesetId']}, "
              f"{len(doc['decorations'])} decors")


def extract_skirmish():
    print("\nDecoding skirmish maps (map<N>)...")
    names = sorted((f for f in os.listdir(SRC_DIR) if re.fullmatch(r"map\d+", f)), key=number_of)
    if not names:
        print("  No map<N> files in this build")
    for name in names:
        with open(os.path.join(SRC_DIR, name), "rb") as fh:
            data = fh.read()
        try:
            doc, end = decode_map(data, 3)  # 3-byte header, then the record, then level data (ignored)
        except IndexError:
            print(f"  Warning: {name} is not a map record, skipped")
            continue
        out = f"map_skirmish_{number_of(name)}"
        with open(os.path.join(OUT_DIR, "maps", f"{out}.json"), "w", encoding="utf-8") as fh:
            json.dump({"name": out, **doc}, fh, indent=2)
        print(f"  Decoded {name} -> {out}.json: {doc['width']}x{doc['height']}, tileset={doc['tilesetId']}, "
              f"{len(doc['decorations'])} decors, trailing={len(data) - end}")


def extract_missions():
    print("\nDecoding the mission list and mission scripts (mi<N>)...")
    ml_path = os.path.join(SRC_DIR, "ml")
    ml_data = []
    if os.path.exists(ml_path):
        with open(ml_path, "rb") as fh:
            data = fh.read()
        count = data[0]
        off = 1
        for _ in range(count):
            arr_len = struct.unpack(">h", data[off:off + 2])[0]
            off += 2
            d_shorts = list(struct.unpack(f">{arr_len}h", data[off:off + arr_len * 2]))
            off += arr_len * 2
            e_shorts = list(struct.unpack(">30h", data[off:off + 60]))
            off += 60
            ml_data.append({"d": d_shorts, "e": e_shorts})
        with open(os.path.join(OUT_DIR, "data", "mission_list.json"), "w", encoding="utf-8") as fh:
            json.dump(ml_data, fh, indent=2)

    # Decode Missions mi0..mi7
    mi_files = [f for f in os.listdir(SRC_DIR) if re.fullmatch(r"mi\d+", f)]
    for mif in sorted(mi_files, key=lambda f: int(f[2:])):
        mipath = os.path.join(SRC_DIR, mif)
        with open(mipath, "rb") as fh:
            mibytes = fh.read()

        ioff = 0
        def read_mi(n):
            nonlocal ioff
            val = 0
            for _ in range(n):
                val = (val << 8) | mibytes[ioff]
                ioff += 1
            return val

        map_index = read_mi(1)
        entity_count = read_mi(1)
        spawns = []
        for _ in range(entity_count):
            etype = read_mi(1)
            ex = read_mi(1)
            ey = read_mi(1)
            edir = read_mi(1)
            spawns.append({
                "type": etype,
                "x": ex,
                "y": ey,
                "direction": edir,
                "faction": etype // 8 if etype < 15 else 0
            })

        build_zones = []
        for _ in range(2):
            grp = []
            for _ in range(3):
                cnt = read_mi(1)
                grp.append([[read_mi(1), read_mi(1), read_mi(1), read_mi(1)] for _ in range(cnt)])
            build_zones.append(grp)

        mine_zones = []
        for _ in range(3):
            cnt = read_mi(1)
            mine_zones.append([[read_mi(1), read_mi(1), read_mi(1), read_mi(1)] for _ in range(cnt)])

        wall_zones = []
        for _ in range(3):
            cnt = read_mi(1)
            wall_zones.append([[read_mi(1), read_mi(1), read_mi(1), read_mi(1)] for _ in range(cnt)])

        patrol_paths = []
        for _ in range(3):
            y_cnt = read_mi(1)
            g_cnt = read_mi(1)
            g_vals = [read_mi(1) for _ in range(g_cnt)]
            h_vals = [read_mi(1) for _ in range(y_cnt + 1)]
            patrol_paths.append({"y": y_cnt, "g": g_vals, "h": h_vals})

        rally_points = []
        for _ in range(3):
            cnt = read_mi(1)
            rally_points.append([[read_mi(1), read_mi(1)] for _ in range(cnt)])

        defense_zones = []
        for _ in range(3):
            cnt = read_mi(1)
            defense_zones.append([[read_mi(1), read_mi(1), read_mi(1), read_mi(1)] for _ in range(cnt)])

        script_count = read_mi(1)
        scripts = []
        for _ in range(script_count):
            scripts.append({
                "cmd": read_mi(1),
                "x": read_mi(1),
                "y": read_mi(1),
                "val": read_mi(1),
                "delay": read_mi(1),
                "group": read_mi(1)
            })

        mission_obj = {
            "mission": mif,
            "mapIndex": map_index,
            "initialEntities": spawns,
            "buildZones": build_zones,
            "mineZones": mine_zones,
            "wallZones": wall_zones,
            "patrolPaths": patrol_paths,
            "rallyPoints": rally_points,
            "defenseZones": defense_zones,
            "scripts": scripts,
            "rawRemainingBytes": list(mibytes[ioff:])
        }
        with open(os.path.join(OUT_DIR, "missions", f"{mif}.json"), "w", encoding="utf-8") as fh:
            json.dump(mission_obj, fh, indent=2)
        print(f"  Decoded mission {mif}: mapIndex={map_index}, initialEntities={len(spawns)}")

def extract_rules():
    print("\nExporting Game Balance Rules and Palettes...")
    # Palette colors from GameConstants
    palettes = {
        "0": {"name": "Default", "rgb": [0, 0, 0], "hex": "#000000"},
        "1": {"name": "Brown", "rgb": [85, 43, 38], "hex": "#552B26"},
        "2": {"name": "Resistance (Player Blue)", "rgb": [100, 147, 195], "hex": "#6493C3"},
        "3": {"name": "Neutral (Green)", "rgb": [125, 215, 116], "hex": "#7DD774"},
        "4": {"name": "Confederation (Enemy Red)", "rgb": [255, 0, 0], "hex": "#FF0000"}
    }
    with open(os.path.join(OUT_DIR, "data", "palettes.json"), "w", encoding="utf-8") as fh:
        json.dump(palettes, fh, indent=2)

    # Unit & weapon definitions from /a (little-endian 16-bit blocks)
    a_path = os.path.join(SRC_DIR, "a")
    d_path = os.path.join(SRC_DIR, "d")
    if os.path.exists(a_path):
        try:
            with open(a_path, "rb") as fh:
                adata = fh.read()

            off = 0
            rule_blocks = []
            for i in range(157):
                b_low = adata[off]
                b_high = adata[off + 1]
                blen = (b_high << 8) | b_low
                off += 2
                block = list(adata[off:off + blen])
                off += blen
                rule_blocks.append(block)

            short_tables = []
            for i in range(9):
                b_low = adata[off]
                b_high = adata[off + 1]
                num_shorts = (b_high << 8) | b_low
                off += 2
                shorts = []
                for _ in range(num_shorts):
                    s_low = adata[off]
                    s_high = adata[off + 1]
                    off += 2
                    shorts.append((s_high << 8) | s_low)
                short_tables.append(shorts)

            # Append extra blocks from /d (blocks 157..159)
            if os.path.exists(d_path):
                with open(d_path, "rb") as fh:
                    ddata = fh.read()
                # 4 header bytes skipped
                doff = 4
                d_lengths = [47, 46, 129]
                for dlen in d_lengths:
                    block = list(ddata[doff:doff + dlen])
                    doff += dlen
                    rule_blocks.append(block)

            balance_obj = {
                "totalRuleBlocks": len(rule_blocks),
                "ruleBlocks": rule_blocks,
                "shortTables": short_tables
            }
            with open(os.path.join(OUT_DIR, "data", "game_balance.json"), "w", encoding="utf-8") as fh:
                json.dump(balance_obj, fh, indent=2)
            print(f"  Exported {len(rule_blocks)} rule blocks and {len(short_tables)} short tables to assets/data/game_balance.json")
        except (IndexError, struct.error):
            print("  Warning: /a has another layout in this build, game_balance.json skipped")


# English titles = gettext msgids of the maps; the Russian originals from s0 are translated by po/ru.po
TITLES_EN = {'Пукальпа': 'Pukalpa', 'Берег Укаяли': 'Ukayali Shore', 'Захват Мигеля': 'Capture of Miguel', 'Застава': 'Outpost', 'Мясорубка': 'Meat Grinder', 'Штурм в ущелье': 'Gorge Assault', 'Последний бой': 'Last Stand', 'Демо-карта': 'Demo Map', 'Счет на секунды': 'Matter of Seconds', 'Радиостанция': 'Radio Station', 'Возвращение': 'The Return', 'Джунгли': 'Jungle', 'Диверсия': 'Sabotage', 'Ущелье': 'The Gorge', 'Лима': 'Lima'}


def mission_titles():
    """{map index: [mission names]} of the build: mi<k> starts with the index of its map, the names are in 0/s0"""
    s0 = os.path.join(SRC_DIR, "0", "s0")
    if not os.path.exists(s0):
        return {}
    with open(s0, "rb") as fh:
        strings = [part.decode("windows-1251", errors="replace") for part in fh.read().split(b"|")]
    if "Миссия" not in strings:
        return {}
    # "Миссия 1" .. "Миссия 7", "Миссия", then the names of the missions in order
    start = strings.index("Миссия") + 1
    missions = sorted(number_of(n) for n in os.listdir(SRC_DIR) if re.fullmatch(r"mi\d+", n))
    titles = {}
    for mission in missions:
        with open(os.path.join(SRC_DIR, f"mi{mission}"), "rb") as fh:
            map_index = fh.read(1)[0]
        if start + mission >= len(strings):
            continue
        name = strings[start + mission]
        titles.setdefault(map_index, []).append(TITLES_EN.get(name, name))
    return titles


def set_title(path, title):
    with open(path, encoding="utf-8") as fh:
        doc = json.load(fh)
    updated = {}
    for key, value in doc.items():
        if key != "title":
            updated[key] = value
        if key == "name":
            updated["title"] = title
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(updated, fh, indent=2, ensure_ascii=False)


def set_map_titles():
    print("\nTitling the campaign maps...")
    for map_index, titles in sorted(mission_titles().items()):
        path = os.path.join(OUT_DIR, "maps", f"{mission_map_name(f'm{map_index}')}.json")
        if os.path.exists(path):
            set_title(path, " / ".join(titles))
            print(f"  m{map_index} -> {os.path.basename(path)}: {' / '.join(titles)}")


# Tile variation ranges [first, last) per tileset (TileMap::loadMap, mission_list.json)
TILE_CLASSES = [
    [(0, 10), (10, 18), (18, 19), (19, 26), (26, 34)],
    [(0, 10), (10, 18), (18, 26), (26, 33), (33, 36)],
]
TILE_COUNT = 245
BLOCKED_SHARE = 0.5


def blocked_cells(doc):
    width, height = doc["width"], doc["height"]
    grid = [[False] * width for _ in range(height)]
    for y, row in enumerate(doc["collisionMask"]):
        for chunk, value in enumerate(row):
            for bit in range(32):
                x = chunk * 32 + bit
                if x < width and value & (1 << (31 - bit)):
                    grid[y][x] = True
    return grid


def decor_cells(doc, graphics, widths, heights):
    tileset = graphics["tilesets"][doc["tilesetId"]]
    offset = graphics["decor_offsets"][doc["tilesetId"]]
    cells = set()
    for decor in doc["decorations"]:
        building = tileset[offset + decor["type"]]
        for y in range(decor["y"], decor["y"] + heights[building]):
            for x in range(decor["x"], decor["x"] + widths[building]):
                cells.add((x, y))
    return cells


def bare_earth_tiles(graphics, render_composite):
    """Tiles whose four quadrants are all brown, per tileset"""
    result = []
    for tileset in (0, 1):
        earth = set()
        for tile, composite in enumerate(graphics["tilesets"][tileset][:TILE_COUNT]):
            image = render_composite(composite)
            image = image.crop((2, 2, image.width - 2, image.height - 2))
            width, height = image.size
            pixels = image.load()
            brown = 0
            for qy in range(2):
                for qx in range(2):
                    r = g = n = 0
                    for y in range(qy * height // 2, (qy + 1) * height // 2):
                        for x in range(qx * width // 2, (qx + 1) * width // 2):
                            p = pixels[x, y]
                            if p[3] > 0:
                                r += p[0]
                                g += p[1]
                                n += 1
                    brown += 1 if n and r > g + 5 * n else 0
            if brown == 4:
                earth.add(tile)
        result.append(earth)
    return result


def to_ranges(values):
    ranges = []
    for value in sorted(values):
        if ranges and value == ranges[-1][1] + 1:
            ranges[-1][1] = value
        else:
            ranges.append([value, value])
    return ranges


def derive_tile_collision():
    """Which ground tiles are impassable, from the collision masks of the destination's map_*.json.

    Passability belongs to the tile sprite: the mask of these maps blocks the brown bare-earth tiles and the jungle /
    rock tiles, while grass, the grass/earth transition tiles and the rubble tiles stay open. Every tile of both
    tilesets gets a strict yes/no answer: the tiles of one variation range share the answer taken from all their cells,
    a tile or class is blocked when at least half of its cells outside decoration footprints are blocked, and a tile
    never seen in the maps is decided by its picture (bare earth in all four quadrants blocks)."""
    print("\nDeriving tile passability...")
    import glob
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import generate_view_gallery as gallery
    gallery.ASSETS_DIR = OUT_DIR
    paths = sorted(glob.glob(os.path.join(OUT_DIR, "maps", "map_*.json")))
    if not paths:
        print("  No map_*.json in the destination (unpack the maps with --game-names / skirmish first)")
        return
    sprites, graphics, sheets = gallery.load_data()
    _, _, render_composite, _, widths, heights, _ = gallery.make_renderer(sprites, graphics, sheets)

    total = [[0] * TILE_COUNT for _ in range(2)]
    blocked = [[0] * TILE_COUNT for _ in range(2)]
    used = []
    for path in paths:
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
        used.append(os.path.basename(path))
        width, height = doc["width"], doc["height"]
        tiles = []
        for run in doc["terrainRle"]:
            tiles += [run["tileType"]] * run["runLength"]
        tiles += [0] * (width * height - len(tiles))
        mask = blocked_cells(doc)
        covered = decor_cells(doc, graphics, widths, heights)
        tileset = doc["tilesetId"]
        for y in range(height):
            for x in range(width):
                if (x, y) in covered:
                    continue
                tile = tiles[y * width + x]
                total[tileset][tile] += 1
                blocked[tileset][tile] += mask[y][x]

    earth = bare_earth_tiles(graphics, render_composite)
    out = {"source": "collisionMask of " + ", ".join(used), "tile_count": TILE_COUNT, "tilesets": []}
    for tileset in (0, 1):
        # group = the tiles sharing one answer: a variation range, or a single tile
        groups = list(TILE_CLASSES[tileset])
        in_class = {tile for first, last in groups for tile in range(first, last)}
        groups += [(tile, tile + 1) for tile in range(TILE_COUNT) if tile not in in_class]
        result = set()
        guessed = 0
        for first, last in groups:
            count = sum(total[tileset][tile] for tile in range(first, last))
            hits = sum(blocked[tileset][tile] for tile in range(first, last))
            if count >= 1:
                verdict = hits >= BLOCKED_SHARE * count
            else:
                verdict = all(tile in earth[tileset] for tile in range(first, last))
                guessed += last - first if verdict else 0
            if verdict:
                result.update(range(first, last))
        out["tilesets"].append({"blocked": to_ranges(result)})
        print(f"  tileset {tileset}: {len(result)} of {TILE_COUNT} tiles blocked ({guessed} by picture), "
              f"ranges {to_ranges(result)}")

    with open(os.path.join(OUT_DIR, "data", "tile_collision.json"), "w", encoding="utf-8") as fh:
        json.dump(out, fh, indent=2)
        fh.write("\n")
    print("  maps:", len(used))


# name -> function; the default run does them in this order
STAGES = {
    "audio": extract_audio,
    "sprites": extract_sprites,
    "graphics": extract_sprite_definitions_and_d0,
    "tilesets": extract_tilesets_and_ui,
    "strings": extract_strings,
    "maps": extract_maps,
    "skirmish": extract_skirmish,
    "missions": extract_missions,
    "rules": extract_rules,
    "titles": set_map_titles,
    "collision": derive_tile_collision,
}
DEFAULT_STAGES = [name for name in STAGES if name != "collision"]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Unpack the game data of an original build into assets (see the module help for the stages).")
    parser.add_argument("--from", dest="src", default=SRC_DIR, metavar="PATH",
                        help="the original build: an unpacked directory or an archive (.jar/.im/.zip) "
                             "(default: %(default)s)")
    parser.add_argument("--dest", dest="dest", default=OUT_DIR, metavar="DIR",
                        help="assets directory to write to (default: %(default)s)")
    parser.add_argument("--only", metavar="STAGES", help="comma-separated stages to run: " + ", ".join(STAGES))
    parser.add_argument("--skip", metavar="STAGES", help="comma-separated stages to leave out of the default set")
    parser.add_argument("--game-names", action="store_true",
                        help="name the campaign maps map_mission_<N+1>.json like the game does (default: m<N>.json)")
    args = parser.parse_args()
    for option in (args.only, args.skip):
        for name in filter(None, (option or "").split(",")):
            if name not in STAGES:
                parser.error(f"unknown stage '{name}' (stages: {', '.join(STAGES)})")
    return args


def run(args):
    global SRC_DIR, OUT_DIR, GAME_NAMES
    SRC_DIR, OUT_DIR, GAME_NAMES = args.src, args.dest, args.game_names
    if args.only:
        stages = [n for n in STAGES if n in args.only.split(",")]
    else:
        skipped = (args.skip or "").split(",")
        stages = [n for n in DEFAULT_STAGES if n not in skipped]
    if not os.path.isdir(SRC_DIR):
        raise SystemExit(f"Source directory not found: {SRC_DIR}")
    print("==================================================")
    print("Art of War 2 - Asset Unpacker")
    print("==================================================")
    setup_dirs()
    for name in stages:
        STAGES[name]()
    print("\n==================================================")
    print("Asset Extraction Complete!")
    print("==================================================")


def main():
    args = parse_args()
    if os.path.isfile(args.src):
        # An archive: unpack it next to nothing permanent
        if not zipfile.is_zipfile(args.src):
            raise SystemExit(f"Not a zip/jar archive: {args.src}")
        with tempfile.TemporaryDirectory(prefix="unpackassets-") as tmp:
            with zipfile.ZipFile(args.src) as archive:
                archive.extractall(tmp)
            args.src = tmp
            run(args)
    else:
        run(args)


if __name__ == "__main__":
    main()
