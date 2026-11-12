#!/usr/bin/env python3
"""Regroups the sprite sheets of assets/sprites by what they are for.

  1. misc:      every sprite_misc_sheet_NN and sprite_misc_unused become ONE sprite, "misc".
  2. icons:     every frame an icon part of icons_conf / icons_res draws is put into that icon sheet (a frame both
                factions draw is copied, the other one stays where it is while somebody else still draws it).
  3. buildings: the frames, parts and animations of the shared building sheets (shared_buildings, shared_fx,
                shared_units) that only ONE building draws are put into that building's own sprite.

Frames are cut out of their sheet (resistance_image variants too), the touched sheets are packed anew and every
"<sheet>.<name>" reference of every meta is rewritten. Frames keep their name (the original sprite id), parts and
animations too. Sheets nobody touched stay byte for byte.

Usage: regroup_sprites.py <sprites dir> [--apply]
Without --apply nothing is written: the plan is printed.
"""
import collections
import glob
import os
import re
import sys

import yaml
from PIL import Image

REF_KEYS = ("frame", "part", "anim", "body")
MAX_W = 2048
PAD = 1
SHEET_KINDS = ("frames", "parts", "anims", "bodies", "sequences", "buildings")
FAMILY_KIND = {"parts": "parts", "frames": "frames", "anims": "anims", "sequences": "sequences", "bodies": "bodies"}
# Sheets that hold things many objects draw: never an owner
SHARED = re.compile(r"^(shared_|misc|tileset_|fx_|projectile_|ui_|construction|icons_)")
BUILDING_SOURCES = ("shared_buildings", "shared_fx", "shared_units")


class Sheet:
    def __init__(self, d, name):
        self.d = d
        self.name = name
        self.path = os.path.join(d, f"sprite_{name}_meta.yaml")
        with open(self.path, encoding="utf-8") as f:
            self.meta = yaml.safe_load(f) or {}
        self.img = None
        self.res = None
        self.touched = False
        self.loadImages()

    def imagePath(self, key, default):
        v = self.meta.get(key, default)
        return os.path.join(self.d, v) if v else None

    def loadImages(self):
        if self.meta.get("frames") is None:
            return
        p = self.imagePath("image", f"sprite_{self.name}.png")
        if p and os.path.exists(p):
            self.img = Image.open(p).convert("RGBA")
        r = self.imagePath("resistance_image", None)
        if r and os.path.exists(r):
            self.res = Image.open(r).convert("RGBA")

    # ---- frames --------------------------------------------------------------------------------------------------
    def frame(self, name):
        v = self.meta["frames"][name]
        rect = list(v["rect"] if isinstance(v, dict) else v)
        attrs = {k: x for k, x in v.items() if k not in ("rect",)} if isinstance(v, dict) else {}
        return rect, attrs

    def scaleOf(self, img):
        iw, ih = self.meta.get("image_size") or img.size
        return img.size[0] / iw, img.size[1] / ih

    def tile(self, name, res=False):
        rect, attrs = self.frame(name)
        img = (self.res if res and self.res is not None else self.img)
        if img is None:
            return None
        sx, sy = self.scaleOf(img)
        x, y, w, h = rect
        box = (round(x * sx), round(y * sy), round((x + w) * sx), round((y + h) * sy))
        return img.crop(box)

    def effectiveAttrs(self, name):
        """frame attributes with the sheet's defaults baked in (so the frame may live in any sheet)"""
        rect, attrs = self.frame(name)
        out = dict(attrs)
        if "size" not in out:
            scale = float(self.meta.get("scale", 1))
            out["size"] = [round(rect[2] / scale, 5), round(rect[3] / scale, 5)]
        if "recolor" not in out and self.meta.get("recolor", "none") != "none":
            out["recolor"] = self.meta["recolor"]
        return out


class World:
    def __init__(self, d):
        self.d = d
        self.sheets = {}
        for f in sorted(glob.glob(os.path.join(d, "sprite_*_meta.yaml"))):
            n = os.path.basename(f)[7:-10]
            self.sheets[n] = Sheet(d, n)
        for s in self.sheets.values():
            self.walk(s.meta, lambda ref, s=s: self.qualify(s, ref), setter=True)
        self.tiles = {}  # (sheet, frame) -> (tile, resTile, attrs) for frames that moved
        self.renames = []  # (old ref, new ref, only these sheets or None): applied to the text of untouched sheets

    # ---- references ----------------------------------------------------------------------------------------------
    @staticmethod
    def qualify(sheet, ref):
        return ref if "." in ref else f"{sheet.name}.{ref}"

    def walk(self, node, fn, setter=False):
        """calls fn(ref) -> new ref on every reference value; rewrites it when setter"""
        if isinstance(node, dict):
            for k, v in list(node.items()):
                if k in REF_KEYS and isinstance(v, str):
                    new = fn(v)
                    if setter:
                        node[k] = new
                else:
                    self.walk(v, fn, setter)
        elif isinstance(node, list):
            for v in node:
                self.walk(v, fn, setter)

    def families(self, sheet):
        return sheet.meta.get("families") or {}

    def pinned(self, sheet):
        """names a family lists: the game finds them by position, they stay in their sheet"""
        out = set()
        for fam in self.families(sheet).values():
            for kind, names in fam.items():
                for n in names or []:
                    out.add((FAMILY_KIND.get(kind, kind), n))
        return out

    def memberOf(self, ref):
        """"sheet.name[i]" family reference -> ("sheet", kind, name) (or None when it is not one)"""
        m = re.match(r"^(.+?)\.([A-Za-z0-9_]+)\[(\d+)\]$", ref)
        if not m:
            return None
        sh, fam, i = self.sheets.get(m.group(1)), m.group(2), int(m.group(3))
        if not sh:
            return None
        f = self.families(sh).get(fam)
        if not f:
            return None
        for kind, names in f.items():
            if names and i < len(names):
                return sh.name, FAMILY_KIND.get(kind, kind), names[i]
        return None

    def resolve(self, key, ref):
        """a reference -> (sheet, kind, name)"""
        kind = {"frame": "frames", "part": "parts", "anim": "anims", "body": "bodies"}[key]
        mem = self.memberOf(ref)
        if mem:
            return mem
        sh, _, name = ref.rpartition(".")
        return sh, kind, name

    def children(self, sheet, kind, name):
        """what one entry draws"""
        entry = (sheet.meta.get(kind) or {}).get(name)
        out = []

        def collect(node):
            if isinstance(node, dict):
                for k, v in node.items():
                    if k in REF_KEYS and isinstance(v, str):
                        out.append(self.resolve(k, v))
                    else:
                        collect(v)
            elif isinstance(node, list):
                for v in node:
                    collect(v)

        collect(entry)
        return out

    # ---- graph ---------------------------------------------------------------------------------------------------
    def consumers(self):
        """(sheet, kind, name) -> set of the sheets that draw it, directly or through other entries"""
        parents = collections.defaultdict(set)  # child -> {(sheet, kind, name)}
        for s in self.sheets.values():
            for kind in SHEET_KINDS:
                for name in s.meta.get(kind) or {}:
                    for ch in self.children(s, kind, name):
                        parents[ch].add((s.name, kind, name))
        memo = {}

        def owners(node, stack=()):
            if node in memo:
                return memo[node]
            if node in stack:
                return set()
            res = set()
            for p in parents.get(node, ()):
                if p[0] != node[0] or p[1] in ("buildings", "bodies", "sequences"):
                    res.add(p[0])
                res |= owners(p, stack + (node,))
            memo[node] = res
            return res

        return owners, parents

    # ---- moving --------------------------------------------------------------------------------------------------
    def addFrame(self, src, name, dst):
        """a frame (with its pixels) into dst; returns False when dst already has it"""
        d = self.sheets[dst]
        frames = d.meta.setdefault("frames", {})
        if name in frames:
            return False
        key = (src, name)
        attrs = self.sheets[src].effectiveAttrs(name)
        if "recolor" not in attrs and d.meta.get("recolor", "none") != "none":
            attrs["recolor"] = "none"
        a, b = self.sheets[src].tile(name), self.sheets[src].tile(name, res=True)
        frames[name] = {"rect": None, **attrs}
        self.tiles[(dst, name)] = (a, b if self.sheets[src].res is not None else None)
        d.touched = True
        return True

    def retarget(self, ref, old, new, only=None):
        """rewrite one reference everywhere (only: the sheets whose references change)"""
        for s in self.sheets.values():
            if only is not None and s.name not in only:
                continue
            self.walk(s.meta, lambda r: new if r == old else r, setter=True)

    def moveNode(self, src, kind, name, dst, copyFor=None):
        """moves (or, with copyFor, copies for those sheets only) one frame / part / anim of src into dst"""
        s, d = self.sheets[src], self.sheets[dst]
        key = {"frames": "frame", "parts": "part", "anims": "anim"}[kind]
        entry = s.meta[kind][name]
        if kind == "frames":
            if name in (d.meta.get("frames") or {}):
                pass
            else:
                self.addFrame(src, name, dst)
        else:
            tgt = d.meta.setdefault(kind, {})
            if name in tgt and tgt[name] != entry:
                raise SystemExit(f"{dst}.{name} exists with other content")
            tgt[name] = entry
            d.touched = True
        old, new = f"{src}.{name}", f"{dst}.{name}"
        self.retarget(old, old, new, only=copyFor)
        self.renames.append((old, new, copyFor))
        if copyFor is None:
            del s.meta[kind][name]
        s.touched = True

    def referenced(self, src, kind, name):
        key = {"frames": "frame", "parts": "part", "anims": "anim"}[kind]
        ref = f"{src}.{name}"
        found = []
        for s in self.sheets.values():
            self.walk(s.meta, lambda r: found.append(s.name) if r == ref else r)
        return found

    # ---- writing -------------------------------------------------------------------------------------------------
    def pack(self, sheet):
        """one atlas out of every frame of a touched sheet"""
        meta = sheet.meta
        frames = meta.get("frames") or {}
        for n in list(frames):  # the sheet's own defaults (scale, recolor) go into the frames: the sheet loses them
            if (sheet.name, n) not in self.tiles:
                rect, _ = sheet.frame(n)
                frames[n] = {"rect": rect, **sheet.effectiveAttrs(n)}
        items = []  # (name, tile, resTile)
        for n in frames:
            if (sheet.name, n) in self.tiles:
                t, r = self.tiles[(sheet.name, n)]
            else:
                t = sheet.tile(n)
                r = sheet.tile(n, res=True) if sheet.res is not None else None
            if t is None:
                raise SystemExit(f"{sheet.name}.{n}: no pixels")
            items.append((n, t, r))
        # frames of the same pixels share one place
        uniq = {}
        for n, t, r in items:
            key = (t.size, t.tobytes(), r.tobytes() if r is not None else None)
            uniq.setdefault(key, []).append(n)
        order = sorted(uniq.items(), key=lambda kv: (-kv[0][0][1], -kv[0][0][0], kv[1][0]))
        x = y = rowH = 0
        width = 0
        place = {}
        for (size, _, _), names in order:
            w, h = size
            if x + w + PAD > MAX_W and x > 0:
                x, y, rowH = 0, y + rowH + PAD, 0
            place[names[0]] = (x, y)
            for n in names[1:]:
                place[n] = (x, y)
            x += w + PAD
            rowH = max(rowH, h)
            width = max(width, x)
        height = y + rowH
        atlas = Image.new("RGBA", (max(1, width), max(1, height)), (0, 0, 0, 0))
        resAtlas = Image.new("RGBA", atlas.size, (0, 0, 0, 0)) if any(r is not None for _, _, r in items) else None
        for n, t, r in items:
            px, py = place[n]
            atlas.paste(t, (px, py))
            if resAtlas is not None:
                resAtlas.paste(r if r is not None else t, (px, py))
            attrs = frames[n]
            frames[n] = {"rect": [px, py, t.size[0], t.size[1]], **{k: v for k, v in attrs.items() if k != "rect"}}
        meta["image"] = f"sprite_{sheet.name}.png"
        meta["image_size"] = [atlas.size[0], atlas.size[1]]
        meta.pop("scale", None)
        meta.pop("recolor", None)
        if resAtlas is not None:
            meta["resistance_image"] = f"sprite_{sheet.name}_resistance.png"
        else:
            meta.pop("resistance_image", None)
        return atlas, resAtlas

    def write(self, sheet, apply):
        meta = sheet.meta
        atlas = resAtlas = None
        if meta.get("frames"):
            atlas, resAtlas = self.pack(sheet)
        elif "frames" in meta:
            meta.pop("frames")
            for k in ("image", "image_size", "scale", "recolor", "resistance_image"):
                meta.pop(k, None)
        # references of the sheet itself stay unqualified
        self.walk(meta, lambda r: r[len(sheet.name) + 1:] if r.startswith(sheet.name + ".") else r, setter=True)
        text = emit(sheet.name, meta)
        if not apply:
            return
        with open(os.path.join(self.d, f"sprite_{sheet.name}_meta.yaml"), "w", encoding="utf-8") as f:
            f.write(text)
        if atlas is not None:
            atlas.save(os.path.join(self.d, meta["image"]), optimize=True)
        if resAtlas is not None:
            resAtlas.save(os.path.join(self.d, meta["resistance_image"]), optimize=True)


# ---- yaml out ------------------------------------------------------------------------------------------------------
def scalar(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, float):
        return f"{v:.5f}".rstrip("0").rstrip(".")
    if isinstance(v, str):
        return v if re.match(r"^[A-Za-z_][A-Za-z0-9_.]*$", v) else "'" + v.replace("'", "''") + "'"
    return str(v)


def flow(v):
    if isinstance(v, dict):
        return "{" + ", ".join(f"{k}: {flow(x)}" for k, x in v.items()) + "}"
    if isinstance(v, list):
        return "[" + ", ".join(flow(x) for x in v) + "]"
    return scalar(v)


def block(v, ind, out):
    pad = "  " * ind
    if isinstance(v, dict):
        for k, x in v.items():
            if isinstance(x, (dict, list)) and not simple(x):
                out.append(f"{pad}{k}:")
                block(x, ind + 1, out)
            else:
                out.append(f"{pad}{k}: {flow(x)}")
    elif isinstance(v, list):
        for x in v:
            if isinstance(x, (dict, list)) and not simple(x):
                out.append(f"{pad}-")
                block(x, ind + 1, out)
            else:
                out.append(f"{pad}- {flow(x)}")


def simple(v):
    """short enough for one line"""
    if isinstance(v, dict):
        return all(not isinstance(x, (dict, list)) or (isinstance(x, list) and all(not isinstance(y, (dict, list)) for y in x)) for x in v.values()) and len(flow(v)) < 160
    if isinstance(v, list):
        return all(not isinstance(x, (dict, list)) for x in v) and len(flow(v)) < 160
    return True


def emit(name, meta):
    out = [f'# Sprite "{name}" (regrouped by tools/regroup_sprites.py)']
    for k, v in meta.items():
        if isinstance(v, (dict, list)) and not (k in ("image_size",) or simple(v) and k not in SHEET_KINDS + ("families",)):
            out.append(f"{k}:")
            block(v, 1, out)
        else:
            out.append(f"{k}: {flow(v)}")
    return "\n".join(out) + "\n"


# ---- the plan ------------------------------------------------------------------------------------------------------
def planMisc(w):
    names = sorted(n for n in w.sheets if n.startswith("misc_sheet_"))
    if "misc_unused" not in w.sheets:
        return []
    return names


def doMisc(w, log):
    names = planMisc(w)
    # misc_unused becomes misc
    old = w.sheets.pop("misc_unused")
    for p in glob.glob(os.path.join(w.d, "sprite_misc_unused*")):
        pass
    old.name = "misc"
    old.meta["image"] = "sprite_misc.png"
    w.sheets["misc"] = old
    ren = lambda r: "misc." + r[len("misc_unused."):] if r.startswith("misc_unused.") else r
    for s in w.sheets.values():
        w.walk(s.meta, ren, setter=True)
    old.touched = True
    w.renames.append(("misc_unused.", "misc.", None))
    n = 0
    for sn in names:
        s = w.sheets.pop(sn)
        for fname in list(s.meta["frames"]):
            if fname in old.meta["frames"]:
                raise SystemExit(f"misc: frame {fname} twice")
            attrs = s.effectiveAttrs(fname)
            if "recolor" not in attrs and old.meta.get("recolor", "none") != "none":
                attrs["recolor"] = "none"
            old.meta["frames"][fname] = {"rect": None, **attrs}
            w.tiles[("misc", fname)] = (s.tile(fname), s.tile(fname, res=True) if s.res is not None else None)
            n += 1
        log.append(f"misc: {sn} ({len(s.meta['frames'])} frames) -> misc")
    # frames of misc_unused itself keep their pixels in the old image: cut them out before the sheets are repacked
    for fname in list(old.meta["frames"]):
        if ("misc", fname) not in w.tiles:
            w.tiles[("misc", fname)] = (old.tile(fname), old.tile(fname, res=True) if old.res is not None else None)
    return names


def doIcons(w, log):
    for icon in ("icons_conf", "icons_res"):
        s = w.sheets[icon]
        refs = set()
        for pname, entry in (s.meta.get("parts") or {}).items():
            for key, ref in iter_refs(entry):
                if key == "frame" and not ref.startswith(icon + "."):
                    refs.add(ref)
        for ref in sorted(refs):
            src, _, name = ref.rpartition(".")
            others = [x for x in w.referenced(src, "frames", name) if x != icon]
            copy = None if not others else [icon]
            w.moveNode(src, "frames", name, icon, copyFor=copy)
            log.append(f"icons: {ref} -> {icon}" + (" (copy)" if copy else ""))
            # the tile has to be in the right sheet's cache afterwards


def iter_refs(node):
    if isinstance(node, dict):
        for k, v in node.items():
            if k in REF_KEYS and isinstance(v, str):
                yield k, v
            else:
                yield from iter_refs(v)
    elif isinstance(node, list):
        for v in node:
            yield from iter_refs(v)


def doBuildings(w, log):
    owners, parents = w.consumers()
    for src in BUILDING_SOURCES:
        s = w.sheets[src]
        pinned = w.pinned(s)
        moves = []
        for kind in ("parts", "anims", "frames"):
            for name in list(s.meta.get(kind) or {}):
                if (kind, name) in pinned:
                    continue
                own = {o for o in owners((src, kind, name)) if o != src}
                builds = {o for o in own if not SHARED.match(o) and w.sheets[o].meta.get("buildings")}
                if len(builds) > 1 and kind == "frames":
                    # drawn by several buildings (a turret and the radar locator stand on the same base): the one that
                    # owns no picture of its own gets it, the others refer to it
                    bare = [b for b in sorted(builds) if not w.sheets[b].meta.get("frames")]
                    builds = set(bare) if len(bare) == 1 else builds
                if len(builds) == 1:
                    moves.append((kind, name, next(iter(builds))))
        for kind, name, dst in moves:
            log.append(f"building: {src}.{name} ({kind[:-1]}) -> {dst}")
        for kind, name, dst in moves:
            w.moveNode(src, kind, name, dst)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    apply = "--apply" in sys.argv
    w = World(sys.argv[1])
    log = []
    gone = doMisc(w, log)
    doIcons(w, log)
    doBuildings(w, log)
    print("\n".join(log))
    touched = [s for s in w.sheets.values() if s.touched]
    print(f"{len(log)} moves; sheets written: {', '.join(s.name for s in touched)}")
    for s in touched:
        w.write(s, apply)
    # the sheets whose pictures stay: only the references in their text change
    for s in w.sheets.values():
        if s.touched:
            continue
        with open(s.path, encoding="utf-8") as f:
            text = f.read()
        new = text
        for old, to, only in w.renames:
            if only is not None and s.name not in only:
                continue
            pat = re.escape(old) + ("" if old.endswith(".") else r"(?![A-Za-z0-9_])")
            new = re.sub(r"(?<![A-Za-z0-9_.])" + pat, to, new)
        if new != text:
            print(f"references: {s.name}")
            if apply:
                with open(s.path, "w", encoding="utf-8") as f:
                    f.write(new)
    if apply:
        for n in gone + ["misc_unused"]:
            for p in glob.glob(os.path.join(w.d, f"sprite_{n}*")):
                if not p.endswith(".bak"):
                    os.remove(p)


if __name__ == "__main__":
    main()
