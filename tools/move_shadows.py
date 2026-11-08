#!/usr/bin/env python3
"""Moves the shadow frames of unit and building sprites into one shared atlas, shared_shadows.

A shadow is a frame whose visible pixels all have one colour (usually black) and none of them is opaque. Its pixels go
to sprite_shared_shadows.png (created when missing, extended otherwise), the frame to sprite_shared_shadows_meta.yaml
as "<object>_<frame>" (frames with the same pixels become one), every "frame:" reference in the object's meta is
rewritten to "shared_shadows.<name>" and the old place in the object's images is cleared.

Frames other files refer to as "<object>.<frame>" stay where they are, and so do frames whose faction image
(resistance_image) holds other pixels at that place.

Usage: move_shadows.py <sprites dir> [--apply] [--preview out.png]
Without --apply nothing is written: the shadows found are listed (and drawn into the preview).
"""
import glob
import hashlib
import os
import re
import sys

import yaml
from PIL import Image

TARGET = "shared_shadows"
SKIP = re.compile(r"^(fx_|shared_|misc_|icons_|projectile_|tileset_|ui_|construction)")
TOLERANCE = 8  # per channel: still "one colour"
PAD = 2


def meta_path(d, n):
    return os.path.join(d, f"sprite_{n}_meta.yaml")


def frame_rect(v):
    return v["rect"] if isinstance(v, dict) else v


def is_shadow(tile):
    """All visible pixels of one colour, none of them opaque"""
    raw = tile.tobytes()
    px = [tuple(raw[i:i + 4]) for i in range(0, len(raw), 4) if raw[i + 3] > 0]
    if len(px) < 4:
        return False
    if max(p[3] for p in px) >= 255:
        return False
    r0, g0, b0 = px[0][:3]
    return all(abs(p[0] - r0) <= TOLERANCE and abs(p[1] - g0) <= TOLERANCE and abs(p[2] - b0) <= TOLERANCE for p in px)


def fmt(v):
    return f"{v:.4f}".rstrip("0").rstrip(".") if isinstance(v, float) else str(v)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    d = sys.argv[1]
    apply = "--apply" in sys.argv
    preview = sys.argv[sys.argv.index("--preview") + 1] if "--preview" in sys.argv else None

    names = sorted(os.path.basename(f)[7:-10] for f in glob.glob(os.path.join(d, "sprite_*_meta.yaml")))
    metas = {n: yaml.safe_load(open(meta_path(d, n))) or {} for n in names}
    texts = {n: open(meta_path(d, n)).read() for n in names}
    elsewhere = ""
    for f in glob.glob(os.path.join(d, "..", "**", "*.yaml"), recursive=True) + glob.glob(
        os.path.join(d, "..", "**", "*.json"), recursive=True
    ):
        if os.sep + "sprites" + os.sep not in f:
            elsewhere += open(f, errors="ignore").read() + "\n"

    jobs = []  # one per moved frame
    skipped = []
    for obj in names:
        if SKIP.match(obj) or obj == TARGET:
            continue
        m = metas[obj]
        frames = m.get("frames") or {}
        if not frames:
            continue
        img = Image.open(os.path.join(d, m.get("image", f"sprite_{obj}.png"))).convert("RGBA")
        res = Image.open(os.path.join(d, m["resistance_image"])).convert("RGBA") if m.get("resistance_image") else None
        dw, dh = m.get("image_size") or list(img.size)
        kx, ky = img.size[0] / dw, img.size[1] / dh
        scale = float(m.get("scale", 1))
        ext = set(re.findall(r"\b" + re.escape(obj) + r"\.([A-Za-z0-9_]+)", elsewhere))
        for n2 in names:
            if n2 != obj:
                ext |= set(re.findall(r"\b" + re.escape(obj) + r"\.([A-Za-z0-9_]+)", texts[n2]))
        for f, v in frames.items():
            r = frame_rect(v)
            box = (round(r[0] * kx), round(r[1] * ky), round((r[0] + r[2]) * kx), round((r[1] + r[3]) * ky))
            if box[2] <= box[0] or box[3] <= box[1]:
                continue
            tile = img.crop(box)
            if not is_shadow(tile):
                continue
            if f in ext:
                skipped.append(f"{obj}.{f}: referred to from another file")
                continue
            if res is not None and res.crop(box).tobytes() != tile.tobytes():
                skipped.append(f"{obj}.{f}: the faction image differs")
                continue
            size = v.get("size") if isinstance(v, dict) else None
            if not size:
                size = [r[2] / scale, r[3] / scale]
            jobs.append(dict(obj=obj, frame=f, box=box, tile=tile, size=size,
                             palette=v.get("palette") if isinstance(v, dict) else None,
                             digest=hashlib.sha1(tile.tobytes() + repr(tuple(size)).encode()).hexdigest()))

    # The same pixels (at the same drawn size) are one frame
    unique = {}
    for j in jobs:
        if j["digest"] not in unique:
            unique[j["digest"]] = f"{j['obj']}_{j['frame']}"
        j["name"] = unique[j["digest"]]
    by_obj = {}
    for j in jobs:
        by_obj.setdefault(j["obj"], []).append(j)
    for obj, js in by_obj.items():
        print(f"{obj:26s} {len(js):3d} shadow frame(s)")
    print(f"total {len(jobs)} frames, {len(unique)} distinct")
    for s in skipped:
        print("kept:", s)
    if not jobs:
        return

    # Atlas: the existing one extended below, else a new one; shelves of the distinct tiles
    target_meta = meta_path(d, TARGET)
    target_png = os.path.join(d, f"sprite_{TARGET}.png")
    old = Image.open(target_png).convert("RGBA") if os.path.exists(target_png) else None
    tiles = {}
    for j in jobs:
        tiles.setdefault(j["name"], j)
    width = max(old.size[0] if old else 0, 1024, max(j["tile"].size[0] for j in tiles.values()))
    x = y = rowh = 0
    places = {}
    for name, j in sorted(tiles.items(), key=lambda kv: -kv[1]["tile"].size[1]):
        w, h = j["tile"].size
        if x + w > width:
            x, y, rowh = 0, y + rowh + PAD, 0
        places[name] = (x, y)
        x += w + PAD
        rowh = max(rowh, h)
    top = old.size[1] + PAD if old else 0
    atlas = Image.new("RGBA", (width, top + y + rowh), (0, 0, 0, 0))
    if old:
        atlas.paste(old, (0, 0))
    for name, j in tiles.items():
        atlas.paste(j["tile"], (places[name][0], top + places[name][1]))
    if preview:
        atlas.save(preview)
        print("preview:", preview, atlas.size)
    if not apply:
        return

    # The atlas meta
    lines = []
    for name, j in sorted(tiles.items()):
        px, py = places[name]
        w, h = j["tile"].size
        extra = f"size: [{fmt(float(j['size'][0]))}, {fmt(float(j['size'][1]))}]"
        if j["palette"]:
            extra += f", palette: {j['palette']}"
        lines.append(f"  {name}: {{rect: [{px}, {top + py}, {w}, {h}], {extra}}}")
    if os.path.exists(target_meta):
        text = open(target_meta).read()
        text = re.sub(r"(?m)^image_size:.*$", f"image_size: [{atlas.size[0]}, {atlas.size[1]}]", text)
        text = re.sub(r"(?m)^frames:\n", "frames:\n" + "\n".join(lines) + "\n", text, count=1)
    else:
        text = (f'# Sprite "{TARGET}": the shadows of the units and buildings (tools/move_shadows.py)\n'
                f"image: sprite_{TARGET}.png\nimage_size: [{atlas.size[0]}, {atlas.size[1]}]\nframes:\n"
                + "\n".join(lines) + "\n")
    atlas.save(target_png)
    open(target_meta, "w").write(text)

    # The objects: frames out, references to the atlas, the old places cleared
    ref = re.compile(r"\bframe: ('?)([A-Za-z0-9_.]+)(\[[0-9]+\])?'?")
    for obj, js in by_obj.items():
        moved = {j["frame"]: j["name"] for j in js}
        out = []
        in_frames = False
        for line in texts[obj].split("\n"):
            if re.match(r"^[A-Za-z_]+:", line):
                in_frames = line.startswith("frames:")
            if in_frames:
                mm = re.match(r"^  ([A-Za-z0-9_]+):", line)
                if mm and mm.group(1) in moved:
                    continue

            def sub(m_):
                r = m_.group(2)
                if r.startswith(obj + "."):
                    r = r[len(obj) + 1:]
                if "." in r or r not in moved or m_.group(3):
                    return m_.group(0)
                return f"frame: {TARGET}.{moved[r]}"

            out.append(ref.sub(sub, line))
        open(meta_path(d, obj), "w").write("\n".join(out))
        m = metas[obj]
        for image in [m.get("image", f"sprite_{obj}.png")] + ([m["resistance_image"]] if m.get("resistance_image") else []):
            p = os.path.join(d, image)
            im = Image.open(p).convert("RGBA")
            for j in js:
                im.paste((0, 0, 0, 0), j["box"])
            im.save(p)
    print("written")


if __name__ == "__main__":
    main()
