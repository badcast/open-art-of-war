#!/usr/bin/env python3
"""Convert the sheet sprites (data/sprites.json + data/sprites_graphics.json + sprites/sheet_NN.png) into per-object
sprites: sprites/sprite_<name>.png + sprites/sprite_<name>_meta.yaml (format: include/graphics/SpriteRegistry.hpp).

    tools/convert_sprites.py <assets dir> <output dir>

Writes the objects into <output dir>/sprites/ and <output dir>/sprite_id_map.json: where every old id went
("<object>.<item>" or "<object>.<family>[i]"), plus the unit, building, tileset, wreck and projectile objects, for
rewriting the references in the characters and the code.

Ownership: an item belongs to the object that names it explicitly (effects, icons), else to the one object that uses
it, else (used by several) to shared_<category>; nothing-uses-it items go to misc_* objects. Wherever the renderer
computes "base + offset" (directions, turret rotation, wreck stages, smoke frames, tile slots) the object defines a
family, laid out contiguously by the registry.
"""
import json
import math
import os
import re
import sys
from collections import Counter, OrderedDict

import yaml
from PIL import Image

# Original sheet widths (TextureManager::BaseSheetWidth): HD sheet scale = png width / this
BASE_WIDTHS = [240, 179, 206, 229, 200, 274, 268, 240, 77, 77, 107, 153, 91, 112, 223, 154, 193, 158, 135, 73, 65,
               121, 91, 94, 106, 169, 68, 78, 126, 231, 272, 45, 166, 123, 122, 1, 61, 88, 50, 63, 98, 33]
BLUE_PAGES = {5, 7, 8, 9, 10, 11, 14, 15}  # 9: the resistance copy of 8 recolours as 8
RED_PAGES = {6, 12, 13, 16, 17, 29, 31}
RESISTANCE_PAGE_SWAP = (8, 9)  # frames of sheet 8 are drawn from sheet 9 (same layout) for the Resistance
PAD = 2

# Kinds: S raw sprite (frame), C composite (part), A anim, B unit body, Q unit sequence, D building
SECTION = {'S': 'frames', 'C': 'parts', 'A': 'anims', 'B': 'bodies', 'Q': 'sequences', 'D': 'buildings'}
ENTRY_KEY = {'S': 'frame', 'C': 'part', 'A': 'anim', 'B': 'body'}
MAP_KEY = {'S': 'frames', 'C': 'composites', 'A': 'anims', 'B': 'bodies', 'Q': 'sequences', 'D': 'buildings'}
LETTER = {'S': 's', 'C': 'c', 'A': 'a', 'B': 'b', 'Q': 'q', 'D': 'd'}
TURRET_FLAGS = {0, 2, 4, 5}  # body layer flags whose child is rotated (Renderer::DrawUnitWorld, s_b53 > 0)


def cround(v):
    """std::round for the values here (half away from zero)"""
    return int(math.floor(v + 0.5)) if v >= 0 else -int(math.floor(-v + 0.5))


def slug(text):
    return re.sub(r'_+', '_', re.sub(r'[^a-z0-9]', '_', text.lower())).strip('_')


class Data:
    def __init__(self, assets):
        self.assets = assets
        self.frames = json.load(open(os.path.join(assets, 'data', 'sprites.json')))
        self.g = json.load(open(os.path.join(assets, 'data', 'sprites_graphics.json')))
        manifest = yaml.safe_load(open(os.path.join(assets, 'main.yaml')))
        self.sheets = {}
        for path in manifest['sprites']['sheets']:
            page = int(re.findall(r'(\d+)', os.path.basename(path))[0])
            self.sheets[page] = os.path.join(assets, path)
        # the resistance copy of sheet 8 is not in the manifest list on every install
        if 9 not in self.sheets and os.path.exists(os.path.join(assets, 'sprites', 'sheet_09.png')):
            self.sheets[9] = os.path.join(assets, 'sprites', 'sheet_09.png')
        self.images = {}
        self.counts = {'S': len(self.frames)}
        for k, sec in (('C', 'composite_sprites'), ('A', 'anim_sequences'), ('B', 'unit_bodies'),
                       ('Q', 'unit_sequences'), ('D', 'buildings')):
            self.counts[k] = len(self.g[sec]['ranges'])

    def image(self, page):
        if page not in self.images:
            self.images[page] = Image.open(self.sheets[page]).convert('RGBA')
        return self.images[page]

    def has_page(self, page):
        return page in self.sheets

    def scale(self, page):
        base = BASE_WIDTHS[page] if 0 <= page < len(BASE_WIDTHS) else 0
        return self.image(page).width / base if base > 0 else 1.0

    def valid(self, kind, i):
        return 0 <= i < self.counts[kind]

    def entries(self, kind, i):
        """(target kind, target id, raw entry, entry index) of a container"""
        g = self.g
        if kind == 'C':
            sec, lst, code = 'composite_sprites', 'parts', {0: 'S', 1: 'C', 2: 'A'}
        elif kind == 'A':
            sec, lst, code = 'anim_sequences', 'frames', {0: 'S', 1: 'C', 2: 'A'}
        elif kind == 'B':
            sec, lst, code = 'unit_bodies', 'layers', {0: 'S', 1: 'C', 2: 'A'}
        elif kind == 'D':
            sec, lst, code = 'buildings', 'parts', {0: 'S', 1: 'C', 2: 'A'}
        elif kind == 'Q':
            start, end = g['unit_sequences']['ranges'][i]
            return [('B', g['unit_sequences']['steps'][j]['body'], g['unit_sequences']['steps'][j], j)
                    for j in range(start, end + 1)]
        else:
            return []
        start, end = g[sec]['ranges'][i]
        out = []
        for j in range(start, end + 1):
            e = g[sec][lst][j]
            out.append((code[e['is_comp']], e['child'], e, j))
        return out


class Obj:
    def __init__(self, name, category, meta=None):
        self.name = name
        self.category = category      # units / buildings / terrain / fx
        self.explicit = OrderedDict()  # (kind, id) -> item name: owned by this object whatever else uses them
        self.roots = []                # (kind, id): what the object is drawn from
        self.families = OrderedDict()  # name -> (kind, [ids])
        self.meta = meta or {}
        self.items = OrderedDict()     # (kind, id) -> name, filled by ownership
        self.rot_families = OrderedDict()  # (kind, base) -> family name (turret windows)

    def name_item(self, kind, i, name):
        self.explicit[(kind, i)] = name

    def family(self, name, kind, ids):
        self.families[name] = (kind, list(ids))


def find_key(node, key):
    """First value of `key` anywhere in a parsed YAML document"""
    if isinstance(node, dict):
        if key in node:
            return node[key]
        for v in node.values():
            r = find_key(v, key)
            if r is not None:
                return r
    elif isinstance(node, list):
        for v in node:
            r = find_key(v, key)
            if r is not None:
                return r
    return None


def build_objects(d):
    """The objects in priority order (explicit names first)"""
    a = d.assets
    chars = {}
    for f in sorted(os.listdir(os.path.join(a, 'characters'))):
        if f.endswith('.yaml'):
            chars[f] = yaml.safe_load(open(os.path.join(a, 'characters', f)))
    effects = chars.get('effects.yaml', {})
    projectiles = chars.get('projectiles.yaml', {}).get('projectiles', {})
    objs = []
    idmap = {'archetypes': {}, 'building_index': {}, 'tilesets': {}, 'wrecks': {}, 'projectiles': {},
             'effects': {}, 'icons': {}}

    # --- Effects ---
    ex = Obj('fx_explosions', 'fx')
    exp = effects.get('explosions', {})
    for i, an in enumerate(exp.get('vehicle', {}).get('main', [11, 12, 13, 14])):
        ex.name_item('A', an, 'blast_%d' % i)
    for key in ('small', 'power', 'large'):
        an = exp.get('building', {}).get(key)
        if an is not None and ('A', an) not in ex.explicit:
            ex.name_item('A', an, 'building_' + key)
    for an in exp.get('vehicle', {}).get('second', []) + exp.get('building', {}).get('extra_anims', []) + \
            [exp.get('building', {}).get('big_anim', 37)]:
        if ('A', an) not in ex.explicit:
            ex.name_item('A', an, 'a%d' % an)
    objs.append(ex)

    imp = Obj('fx_impacts', 'fx')
    for pname, p in projectiles.items():
        for i, an in enumerate(p.get('impact', []) or []):
            if ('A', an) not in imp.explicit and ('A', an) not in ex.explicit:
                imp.name_item('A', an, '%s_%d' % (pname, i))
        for key in ('impact_wall', 'wall_destroyed'):
            an = p.get(key)
            if an is not None and ('A', an) not in imp.explicit and ('A', an) not in ex.explicit:
                imp.name_item('A', an, 'wall_destroyed' if key == 'wall_destroyed' else '%s_wall' % pname)
    # Effects::ExplosionCentering knows these explosions too
    for an in list(range(15, 21)) + [42] + list(range(48, 59)):
        if d.valid('A', an) and ('A', an) not in imp.explicit and ('A', an) not in ex.explicit:
            imp.name_item('A', an, 'a%d' % an)
    objs.append(imp)

    jet = Obj('fx_flame_jet', 'fx')
    jet.name_item('A', 59, 'jet')
    objs.append(jet)

    for pname, p in projectiles.items():
        base, n = p.get('sprite', 0), p.get('directions', 16)
        if not base or not d.valid('C', base + n - 1):
            continue
        o = Obj('projectile_' + slug(pname), 'fx')
        for i in range(n):
            o.name_item('C', base + i, 'dir_%d' % i)
        o.family('dirs', 'C', range(base, base + n))
        idmap['projectiles'][pname] = {'object': o.name, 'base': base, 'directions': n}
        objs.append(o)

    marks = Obj('fx_craters', 'fx')
    for i, c in enumerate(range(1789, 1801)):
        marks.name_item('C', c, 'mark_%d' % i)
    objs.append(marks)

    smoke = Obj('fx_smoke_puff', 'fx')
    for i, c in enumerate(range(1440, 1454)):
        smoke.name_item('C', c, 'frame_%d' % i)
    smoke.family('frames', 'C', range(1440, 1454))
    objs.append(smoke)

    death = Obj('fx_infantry_death', 'fx')
    for s, n in ((247, 'death_0'), (248, 'death_1'), (259, 'flame_0'), (260, 'flame_1')):
        death.name_item('Q', s, n)
    objs.append(death)

    wr = effects.get('wrecks', {})
    wreck_defs = OrderedDict()
    for key, default in (('vehicle', (1764, 5)), ('light', (1759, 5)), ('heavy', (1769, 4)),
                         ('building_small', (1741, 6)), ('building_medium', (1747, 6)),
                         ('building_large', (1753, 6))):
        w = wr.get(key, {})
        wreck_defs[(w.get('composite', default[0]), w.get('stages', default[1]))] = \
            ('fx_wreck_' + key) if not key.startswith('building') else 'fx_rubble_' + key.split('_')[1]
    for ch in chars.values():
        an = (ch or {}).get('animations') or {}
        if 'wreck_composite' in an:
            key = (an['wreck_composite'], an.get('wreck_stages', 1))
            wreck_defs.setdefault(key, 'fx_wreck_%d' % key[0])
    for (c, n), name in wreck_defs.items():
        o = Obj(name, 'fx')
        for i in range(n):
            o.name_item('C', c + i, 'stage_%d' % i)
        o.family('stages', 'C', range(c, c + n))
        idmap['wrecks'][str(c)] = {'object': name, 'stages': n}
        objs.append(o)

    rubble = Obj('fx_wall_rubble', 'fx')
    rubble.name_item('C', 1778, 'rubble')
    objs.append(rubble)
    marker = Obj('ui_siege_marker', 'fx')
    marker.name_item('C', 1872, 'marker')
    objs.append(marker)
    power = Obj('ui_power_icon', 'fx')
    power.name_item('S', 1778, 'icon')
    objs.append(power)

    # --- Icons: one object per faction (unit cameos, building icons, research icons) ---
    icon_objs = {'res': Obj('icons_res', 'fx'), 'conf': Obj('icons_conf', 'fx')}
    objs += list(icon_objs.values())

    def add_icon(fac, comp, item):
        o = icon_objs[fac]
        if ('C', comp) not in o.explicit:
            o.name_item('C', comp, item)
            idmap['icons'][str(comp)] = '%s.%s' % (o.name, item)

    def char_icon(ch, comp):
        """cameo of a character: icons_<faction>.<id without the faction suffix>"""
        cid = ch['id']
        fac = 'res' if cid.endswith('_res') else 'conf'
        add_icon(fac, comp, re.sub(r'_(res|conf)$', '', cid))

    for f, ch in chars.items():
        if not f.startswith('research_') or not ch:
            continue
        fac = 'res' if slug(ch.get('faction', f[9:-5])).startswith('res') else 'conf'
        for r in ch.get('research', []):
            if 'icon' in r:
                add_icon(fac, r['icon'], 'research_' + slug(r.get('name', str(r.get('id')))))

    # --- Units ---
    hud_cameos = {0: 2057, 2: 2058, 4: 2059, 6: 2060, 8: 2061, 10: 2062, 12: 2063,
                  1: 2049, 3: 2050, 5: 2051, 7: 2052, 9: 2053, 11: 2054, 13: 2056}
    arch = d.g['unit_archetypes']
    units = {}
    forms = {}  # form archetype -> (parent archetype, prefix)
    for ch in chars.values():
        if not ch or ch.get('type') != 'unit' or 'archetype_id' not in ch:
            continue
        units[ch['archetype_id']] = ch
        ab = ch.get('abilities') or {}
        for key, val in ab.items():
            if isinstance(val, dict) and 'archetype_id' in val:
                forms[val['archetype_id']] = (ch['archetype_id'], 'siege_' if key == 'siege' else key + '_')
        if ch.get('baseline_archetype_id') is not None:
            forms[ch['baseline_archetype_id']] = (ch['archetype_id'], 'baseline_')
        for key, val in ch.items():
            if key.endswith('baseline_archetype_id') and isinstance(val, int):
                forms[val] = (ch['archetype_id'], 'baseline_')
    for ch in chars.values():  # baseline declared under abilities
        for key, val in ((ch or {}).get('abilities') or {}).items():
            if isinstance(val, dict) and 'baseline_archetype_id' in val:
                forms[val['baseline_archetype_id']] = (ch['archetype_id'], 'baseline_')
    unit_objs = {}
    for aid in sorted(units):
        ch = units[aid]
        o = Obj(ch['id'], 'units')
        if aid in hud_cameos:
            char_icon(ch, hud_cameos[aid])
        unit_objs[aid] = o
        objs.append(o)
    for aid in range(len(arch)):
        if aid in unit_objs:
            o, prefix = unit_objs[aid], ''
        elif aid in forms and forms[aid][0] in unit_objs:
            o, prefix = unit_objs[forms[aid][0]], forms[aid][1]
        else:
            o = Obj('unit_archetype_%d' % aid, 'units')
            unit_objs[aid], prefix = o, ''
            objs.append(o)
        idmap['archetypes'][str(aid)] = {'object': o.name, 'prefix': prefix}
        row = arch[aid]
        fixed = 14 <= aid <= 16  # deployed siege: one hull facing
        ctx = []  # (family name, [seq ids], transform?)
        for k, base in enumerate(row):
            if k < 2:
                name = 'idle' if k == 0 else 'move'
                if base < 230 and not fixed:
                    ctx.append((name, [base + i for i in range(8)], False))
                    if k == 0 and aid <= 3:
                        ctx.append(('attack', [base + 8 + i for i in range(8)], False))
                else:
                    ctx.append((name, [base], False))
            elif k == 2:
                ctx.append(('die', [base], None))
            else:
                ctx.append(('transform', [base], True))
        for name, seqs, transform in ctx:
            o.family(prefix + name, 'Q', seqs)
            o.roots += [('Q', s) for s in seqs]
            o.meta.setdefault('_contexts', []).append((seqs, transform))

    # --- Buildings ---
    build_chars = {ch['building_index']: ch for ch in chars.values()
                   if ch and ch.get('type') != 'unit' and 'building_index' in ch}
    for b in range(15):
        ch = build_chars.get(b)
        o = Obj(ch['id'] if ch else 'building_%d' % b, 'buildings')
        o.name_item('D', b, 'main')
        icon = find_key(ch, 'icon_composite') if ch else None
        if icon is not None:
            char_icon(ch, icon)
        idmap['building_index'][str(b)] = o.name + '.main'
        objs.append(o)
    for first, name, fallback in ((15, 'wall_conf', 'wall_conf'), (20, 'wall_conf_reinforced', None),
                                  (25, 'wall_res', 'wall_res')):
        o = Obj(name, 'buildings')
        for i in range(5):
            o.name_item('D', first + i, 'damage_%d' % i)
            idmap['building_index'][str(first + i)] = '%s.damage_%d' % (name, i)
        o.family('damage', 'D', range(first, first + 5))
        for ch in build_chars.values():
            icon = find_key(ch, 'icon_composite')
            if ch.get('building_index') == first and icon is not None:
                char_icon(ch, icon)
        objs.append(o)
    sc = Obj('construction_scaffold', 'buildings')
    sc.name_item('D', 30, 'main')
    idmap['building_index']['30'] = 'construction_scaffold.main'
    objs.append(sc)

    # --- Tilesets ---
    for t, lst in enumerate(d.g['tilesets']):
        o = Obj('tileset_%d' % t, 'terrain', {'terrain': True})
        offs = d.g.get('decor_offsets', [])
        off = offs[t] if t < len(offs) else len(lst)
        o.family('tiles', 'C', lst[:off])
        o.family('decor', 'D', lst[off:])
        o.roots += [('C', c) for c in lst[:off]] + [('D', b) for b in lst[off:]]
        idmap['tilesets'][str(t)] = {'object': o.name, 'decor_offset': off}
        objs.append(o)
    return objs, idmap


def closure(d, roots):
    seen = set()
    stack = [r for r in roots if d.valid(*r)]
    while stack:
        k, i = stack.pop()
        if (k, i) in seen or not d.valid(k, i):
            continue
        seen.add((k, i))
        for tk, ti, _, _ in d.entries(k, i):
            if (tk, ti) not in seen and d.valid(tk, ti):
                stack.append((tk, ti))
    return seen


def turret_contexts(d, objs):
    """(body, entry index) -> (kind, base): the rotating layers; (building, entry index) -> (kind, base) likewise"""
    layers = {}
    conflicts = []
    for o in objs:
        for seqs, transform in o.meta.get('_contexts', []):
            if transform is None:
                continue
            for dirn, s in enumerate(seqs):
                if not d.valid('Q', s):
                    continue
                for _, body, _, _ in d.entries('Q', s):
                    if not d.valid('B', body):
                        continue
                    for tk, child, e, j in d.entries('B', body):
                        if e['flag'] not in TURRET_FLAGS:
                            continue
                        base = child if transform else child - dirn
                        key = ('B', body, j)
                        if key in layers and layers[key] != (tk, base):
                            conflicts.append(key)
                        layers[key] = (tk, base)
    for b in range(min(15, d.counts['D'])):
        for tk, child, e, j in d.entries('D', b):
            if e['layer'] == 2 and e['state'] in (1, 2):
                layers[('D', b, j)] = (tk, child)
    return layers, conflicts


def assign_owners(d, objs, layers):
    owner = {}
    # 1. Explicit names
    for o in objs:
        for key, name in o.explicit.items():
            if d.valid(*key) and key not in owner:
                owner[key] = o
                o.items[key] = name
    # 2. Turret windows are drawn too: they belong to what the layer's object reaches
    extra_roots = {}
    for (ck, cid, _), (tk, base) in layers.items():
        extra_roots.setdefault((ck, cid), []).extend((tk, base + i) for i in range(8))
    users = {}
    for o in objs:
        roots = list(o.explicit) + o.roots + [(k, i) for _, (k, ids) in o.families.items() for i in ids]
        cl = closure(d, roots)
        more = set()
        for key in cl:
            for r in extra_roots.get(key, []):
                more |= closure(d, [r])
        for key in cl | more:
            users.setdefault(key, []).append(o)
    shared = {}
    by_name = {o.name: o for o in objs}
    for key in sorted(users, key=lambda k: (k[0], k[1])):
        if key in owner:
            continue
        us = list(OrderedDict((u.name, u) for u in users[key]).values())
        if len(us) == 1:
            o = us[0]
        else:
            cat = us[0].category
            name = 'shared_' + cat
            if name not in by_name:
                o = Obj(name, cat, {'terrain': True} if cat == 'terrain' else {})
                by_name[name] = o
                objs.append(o)
                shared[name] = o
            o = by_name[name]
        owner[key] = o
    # 3. What nothing reaches
    misc = Obj('misc_unused', 'fx')
    for k in ('C', 'A', 'B', 'Q', 'D'):
        for i in range(d.counts[k]):
            if (k, i) not in owner:
                misc.roots.append((k, i))
    if misc.roots:
        objs.append(misc)
        by_name[misc.name] = misc
        for key in closure(d, misc.roots):
            if key not in owner:
                owner[key] = misc
    for f in d.frames:
        key = ('S', f['id'])
        if key in owner or not d.valid(*key):
            continue
        name = 'misc_sheet_%02d' % f['page']
        if name not in by_name:
            by_name[name] = Obj(name, 'fx')
            objs.append(by_name[name])
        owner[key] = by_name[name]
    for key, o in owner.items():
        if key not in o.items:
            o.items[key] = LETTER[key[0]] + str(key[1])
    return owner


def ref(owner, key, from_obj):
    o = owner.get(key)
    if o is None:
        return None
    name = o.items[key]
    return name if o is from_obj else '%s.%s' % (o.name, name)


def family_ref(o, fam, index, from_obj):
    return ('%s[%d]' % (fam, index)) if o is from_obj else '%s.%s[%d]' % (o.name, fam, index)


def pack(sizes, pad):
    """Shelf packing: sizes [(w, h)] -> positions [(x, y)], atlas (W, H)"""
    order = sorted(range(len(sizes)), key=lambda i: (-sizes[i][1], -sizes[i][0]))
    area = sum((w + 2 * pad) * (h + 2 * pad) for w, h in sizes)
    widest = max([w + 2 * pad for w, h in sizes] + [1])
    width = max(widest, min(4096, int(math.ceil(math.sqrt(area) * 1.15))))
    pos = [None] * len(sizes)
    x = y = shelf = 0
    for i in order:
        w, h = sizes[i][0] + 2 * pad, sizes[i][1] + 2 * pad
        if x + w > width:
            x, y, shelf = 0, y + shelf, 0
        pos[i] = (x + pad, y + pad)
        x += w
        shelf = max(shelf, h)
    return pos, (width, max(1, y + shelf))


def crop(d, page, f):
    """The pixels the renderer took for a frame: round(rect * sheet scale); None when it drew nothing (no sheet)"""
    if not d.has_page(page):
        return None
    s = d.scale(page)
    x, y, w, h = cround(f['x'] * s), cround(f['y'] * s), cround(f['width'] * s), cround(f['height'] * s)
    if w <= 0 or h <= 0:
        return None
    return d.image(page).crop((x, y, x + w, y + h))  # PIL pads outside the image with transparency


def paste_extruded(atlas, img, x, y, pad):
    atlas.paste(img, (x, y))
    w, h = img.size
    for p in range(1, pad + 1):
        atlas.paste(img.crop((0, 0, w, 1)), (x, y - p))
        atlas.paste(img.crop((0, h - 1, w, h)), (x, y + h - 1 + p))
    col_l = atlas.crop((x, y - pad, x + 1, y + h + pad))
    col_r = atlas.crop((x + w - 1, y - pad, x + w, y + h + pad))
    for p in range(1, pad + 1):
        atlas.paste(col_l, (x - p, y - pad))
        atlas.paste(col_r, (x + w - 1 + p, y - pad))


def num(v):
    return int(v) if float(v).is_integer() else round(float(v), 4)


class Flow(list):
    pass


class FlowMap(dict):
    pass


def _flow_list(dumper, data):
    return dumper.represent_sequence('tag:yaml.org,2002:seq', data, flow_style=True)


def _flow_map(dumper, data):
    return dumper.represent_mapping('tag:yaml.org,2002:map', data, flow_style=True)


def _odict(dumper, data):
    return dumper.represent_mapping('tag:yaml.org,2002:map', data.items())


yaml.add_representer(Flow, _flow_list)
yaml.add_representer(FlowMap, _flow_map)
yaml.add_representer(OrderedDict, _odict)


def write_object(d, o, owner, layers, out_dir):
    meta = OrderedDict()
    frames = [(key, name) for key, name in o.items.items() if key[0] == 'S']
    crops = []
    for key, name in frames:
        f = d.frames[key[1]]
        crops.append((key, name, f, crop(d, f['page'], f)))
    has_alt = any(f['page'] == RESISTANCE_PAGE_SWAP[0] and c is not None for _, _, f, c in crops)
    sizes = [(c.width, c.height) if c else (0, 0) for _, _, _, c in crops]
    pos, (aw, ah) = pack(sizes, PAD)
    atlas = Image.new('RGBA', (aw, ah), (0, 0, 0, 0))
    alt = Image.new('RGBA', (aw, ah), (0, 0, 0, 0)) if has_alt else None
    for (key, name, f, c), p in zip(crops, pos):
        if c is None:
            continue
        paste_extruded(atlas, c, p[0], p[1], PAD)
        if alt is not None:
            c2 = crop(d, RESISTANCE_PAGE_SWAP[1], f) if f['page'] == RESISTANCE_PAGE_SWAP[0] else c
            paste_extruded(alt, c2 if c2 is not None else c, p[0], p[1], PAD)
    image_name = 'sprite_%s.png' % o.name
    meta['image'] = image_name
    if alt is not None:
        meta['resistance_image'] = 'sprite_%s_resistance.png' % o.name
    if o.meta.get('terrain'):
        meta['terrain'] = True

    def team(page):
        return 'blue' if page in BLUE_PAGES else 'red' if page in RED_PAGES else 'none'
    teams = Counter(team(f['page']) for _, _, f, _ in crops)
    default_team = teams.most_common(1)[0][0] if teams else 'none'
    if default_team != 'none':
        meta['recolor'] = default_team
    if frames:
        fm = OrderedDict()
        for (key, name, f, c), p in zip(crops, pos):
            w, h = (c.width, c.height) if c else (0, 0)
            e = FlowMap()
            e['rect'] = Flow([p[0] if c else 0, p[1] if c else 0, w, h])
            e['size'] = Flow([num(f['width']), num(f['height'])])
            if team(f['page']) != default_team:
                e['recolor'] = team(f['page'])
            if f.get('palette'):
                e['palette'] = f['palette']
            fm[name] = e
        meta['frames'] = fm

    placeholders = OrderedDict()

    def missing(kind, i):
        """A reference the old tables could not resolve (the game drew nothing): an empty local item"""
        placeholders[kind] = 'none_' + LETTER[kind]
        return placeholders[kind]

    def entry(container_kind, cid, tk, child, e, j):
        out = FlowMap()
        lk = (container_kind, cid, j)
        if lk in layers:
            k, base = layers[lk]
            fam = o.rot_families.setdefault((k, base), 'rot_%s%d' % (LETTER[k], base))
            out[ENTRY_KEY[tk]] = '%s[%d]' % (fam, child - base)
        else:
            r = ref(owner, (tk, child), o)
            out[ENTRY_KEY[tk]] = r if r is not None else missing(tk, child)
        ox = e.get('ox', 0)
        oy = e.get('oy', 0)
        if ox or oy:
            out['at'] = Flow([ox, oy])
        for key in ('flag', 'layer', 'state'):
            if e.get(key):
                out[key] = e[key]
        return out

    for kind in ('C', 'A', 'B', 'Q', 'D'):
        items = [(key, name) for key, name in o.items.items() if key[0] == kind]
        if not items:
            continue
        sec = OrderedDict()
        for key, name in items:
            ents = [entry(kind, key[1], tk, child, e, j) for tk, child, e, j in d.entries(kind, key[1])]
            if kind == 'D':
                b = key[1]
                bd = OrderedDict()
                g = d.g['buildings']
                bd['size'] = Flow([g['widths'][b] if b < len(g['widths']) else 1,
                                   g['heights'][b] if b < len(g['heights']) else 1])
                # what the renderer did by position: 0-14 structures, 15-29 walls (army colour, animated parts),
                # 30 (scaffolding) and 31-132 plain decorations, 133+ decorations drawn in one pass
                bd['style'] = 'structure' if b < 15 else 'wall' if b < 30 else 'decor' if b < 133 else 'decor_flat'
                if 31 <= b < 83:
                    bd['tall'] = True
                bd['parts'] = ents
                sec[name] = bd
            else:
                sec[name] = ents
        meta[SECTION[kind]] = sec

    fams = OrderedDict()
    for fname, (kind, ids) in o.families.items():
        members = []
        for i in ids:
            r = ref(owner, (kind, i), o)
            members.append(r if r is not None else missing(kind, i))
        fams[fname] = FlowMap({SECTION[kind]: Flow(members)})
    for (kind, base), fname in o.rot_families.items():
        members = []
        for i in range(base, base + 8):
            r = ref(owner, (kind, i), o)
            members.append(r if r is not None else missing(kind, i))
        fams[fname] = FlowMap({SECTION[kind]: Flow(members)})
    for kind, name in placeholders.items():
        if kind == 'S':
            empty = FlowMap({'rect': Flow([0, 0, 0, 0])})
        elif kind == 'D':
            empty = FlowMap({'size': Flow([1, 1]), 'parts': Flow([])})
        else:
            empty = Flow([])
        meta.setdefault(SECTION[kind], OrderedDict())[name] = empty
    if 'families' in meta:
        del meta['families']
    if fams:
        meta['families'] = fams

    os.makedirs(out_dir, exist_ok=True)
    if frames:
        atlas.save(os.path.join(out_dir, image_name), optimize=True)
        if alt is not None:
            alt.save(os.path.join(out_dir, meta['resistance_image']), optimize=True)
    else:
        del meta['image']
    with open(os.path.join(out_dir, 'sprite_%s_meta.yaml' % o.name), 'w') as fh:
        fh.write('# Sprite "%s" (converted from the sheet sprites by tools/convert_sprites.py)\n' % o.name)
        yaml.dump(meta, fh, sort_keys=False, allow_unicode=True, width=120)
    return len(placeholders)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    assets, out = sys.argv[1], sys.argv[2]
    d = Data(assets)
    objs, idmap = build_objects(d)
    layers, conflicts = turret_contexts(d, objs)
    if conflicts:
        print('error: %d rotating layers used with different bases: %s' % (len(conflicts), conflicts[:5]))
        return 1
    for (ck, cid, j), (k, base) in layers.items():
        if not all(d.valid(k, base + i) for i in range(8)):
            print('warning: rotation window %s%d..+7 leaves the table' % (k, base))
    owner = assign_owners(d, objs, layers)
    for o in objs:
        o.meta.pop('_contexts', None)
    sprite_dir = os.path.join(out, 'sprites')
    missing = 0
    written = 0
    for o in objs:
        if not o.items and not o.families:
            continue
        missing += write_object(d, o, owner, layers, sprite_dir)
        written += 1
    for k in MAP_KEY.values():
        idmap[k] = {}
    for (k, i), o in sorted(owner.items(), key=lambda kv: (kv[0][0], kv[0][1])):
        idmap[MAP_KEY[k]][str(i)] = '%s.%s' % (o.name, o.items[(k, i)])
    with open(os.path.join(out, 'sprite_id_map.json'), 'w') as fh:
        json.dump(idmap, fh, indent=1)
    print('%d objects written to %s (%d objects refer to ids the old tables did not have)' % (written, sprite_dir, missing))
    return 0


if __name__ == '__main__':
    sys.exit(main())
