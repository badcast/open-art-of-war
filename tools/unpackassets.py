#!/usr/bin/env python3
"""
Art of War 2 - unpack the sprites of an original build into assets/.

Writes only the sprite data:
  assets/sprites/sheet_NN.png        sprite sheets (RGBA, the alpha masks are merged in)
  assets/characters/sprites.json     sprite rectangles (page, x, y, width, height, palette)

--from takes the original build as a directory (already unpacked) or an archive (.jar / .im / .zip);
--dest is the assets directory to write to.

Examples:
  unpackassets.py --from original_extracted --dest assets
  unpackassets.py --from AoW2.jar --dest /tmp/assets
"""

import argparse
import io
import json
import os
import struct
import tempfile
import zipfile

from PIL import Image


def extract_sprite_sheets(src_dir, out_dir):
    """i0 holds pairs of chunks: a base image followed by its alpha mask."""
    print("Extracting sprite sheets and merging alpha masks...")
    i0_path = os.path.join(src_dir, "i0")
    if not os.path.exists(i0_path):
        raise SystemExit("Error: i0 not found in the source")

    with open(i0_path, "rb") as fh:
        data = fh.read()

    offset = 0
    chunks = []
    while offset < len(data):
        first = data[offset]
        offset += 1
        if first == 255:
            break
        length = (first << 16) | (data[offset] << 8) | data[offset + 1]
        offset += 2
        chunks.append(data[offset:offset + length])
        offset += length

    sheet_dir = os.path.join(out_dir, "sprites")
    os.makedirs(sheet_dir, exist_ok=True)
    sheet_count = len(chunks) // 2
    print(f"  Loaded {len(chunks)} raw chunks from i0, rebuilding {sheet_count} sheets")
    for page in range(sheet_count):
        base_chunk = chunks[page * 2]
        mask_chunk = chunks[page * 2 + 1]
        if len(base_chunk) == 0:
            continue

        base_img = Image.open(io.BytesIO(base_chunk)).convert("RGBA")
        combined = base_img
        if len(mask_chunk) > 0:
            mask_img = Image.open(io.BytesIO(mask_chunk)).convert("L")
            if mask_img.size == base_img.size:
                r, g, b, _ = base_img.split()
                combined = Image.merge("RGBA", (r, g, b, mask_img))
        combined.save(os.path.join(sheet_dir, f"sheet_{page:02d}.png"))
    print(f"  Exported {sheet_count} RGBA sprite sheets to {sheet_dir}")


def extract_sprite_rectangles(src_dir, out_dir):
    """d0 starts with 26 byte arrays; the first six describe the sprite frames."""
    print("Extracting sprite rectangles...")
    d0_path = os.path.join(src_dir, "d0")
    if not os.path.exists(d0_path):
        raise SystemExit("Error: d0 not found in the source")

    with open(d0_path, "rb") as fh:
        data = fh.read()

    offset = 0
    byte_arrays = []
    for _ in range(6):
        length = struct.unpack(">h", data[offset:offset + 2])[0]
        offset += 2
        byte_arrays.append(data[offset:offset + length])
        offset += length

    pages, xs, ys, widths, heights, palettes = byte_arrays
    sprites = [{
        "id": i,
        "page": pages[i],
        "x": xs[i],
        "y": ys[i],
        "width": widths[i],
        "height": heights[i],
        "palette": palettes[i],
    } for i in range(len(pages))]

    os.makedirs(os.path.join(out_dir, "characters"), exist_ok=True)
    out_path = os.path.join(out_dir, "characters", "sprites.json")
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(sprites, fh, indent=2)
    print(f"  Exported {len(sprites)} sprite definitions to {out_path}")


def run(src_dir, out_dir):
    if not os.path.isdir(src_dir):
        raise SystemExit(f"Source directory not found: {src_dir}")
    extract_sprite_sheets(src_dir, out_dir)
    extract_sprite_rectangles(src_dir, out_dir)
    print("Sprite extraction complete.")


def main():
    parser = argparse.ArgumentParser(description="Unpack the sprites of an original Art of War 2 build into assets.")
    parser.add_argument("--from", dest="src", default="original_extracted", metavar="PATH",
                        help="the original build: an unpacked directory or an archive (.jar/.im/.zip) (default: %(default)s)")
    parser.add_argument("--dest", dest="dest", default="assets", metavar="DIR",
                        help="assets directory to write to (default: %(default)s)")
    args = parser.parse_args()

    if os.path.isfile(args.src):
        if not zipfile.is_zipfile(args.src):
            raise SystemExit(f"Not a zip/jar archive: {args.src}")
        with tempfile.TemporaryDirectory(prefix="unpackassets-") as tmp:
            with zipfile.ZipFile(args.src) as archive:
                archive.extractall(tmp)
            run(tmp, args.dest)
    else:
        run(args.src, args.dest)


if __name__ == "__main__":
    main()
