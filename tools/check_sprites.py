#!/usr/bin/env python3
"""Checks that converted sprites draw exactly what the sheet sprites drew.

    tools/check_sprites.py <assets dir> <converted dir> <openaow-sprite-dump>

<converted dir> is the output of tools/convert_sprites.py (sprites/ and sprite_id_map.json). The metas are compiled by
the game's own SpriteRegistry (through openaow-sprite-dump); then the draw calls of the renderer are replayed on the
old tables and on the new ones, and every resulting sprite draw is compared: position, drawn size, team recolour and
the pixels themselves. Covered: every unit archetype in every action, facing and turret facing, both factions; every
building in every damage state, turret facing and layer; tiles and decorations of every tileset; wreck and rubble
stages; projectile directions; smoke frames; explosions, impacts, icons and every other composite / animation.
"""
import hashlib
import json
import os
import subprocess
import sys
import tempfile

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import convert_sprites as cs  # noqa: E402

RES, CONF = 'res', 'conf'


class Lengths:
    def anim_len(self, i):
        s, e = self.anim_range(i)
        return e - s + 1 if e >= s else 0

    def seq_len(self, i):
        s, e = self.seq_range(i)
        return e - s + 1 if e >= s else 0


class Old(Lengths):
    def __init__(self, d):
        self.d = d
        self.g = d.g
        self.cache = {}

    def team(self, page):
        return 1 if page in cs.BLUE_PAGES or page in (8, 9) else 2 if page in cs.RED_PAGES else 0

    def frame(self, i, faction):
        if not (0 <= i < len(self.d.frames)):
            return None
        f = self.d.frames[i]
        if f['width'] <= 0 or f['height'] <= 0:
            return None
        page = f['page']
        if page == 8 and faction == RES:
            page = 9
        key = (i, page)
        if key not in self.cache:
            c = cs.crop(self.d, page, f)
            self.cache[key] = hashlib.md5(c.tobytes() + bytes(str(c.size), 'ascii')).hexdigest() if c else None
        if self.cache[key] is None:
            return None
        return (self.cache[key], f['width'], f['height'], self.team(f['page']))

    def comp_range(self, i):
        r = self.g['composite_sprites']['ranges']
        return r[i] if 0 <= i < len(r) else (0, -1)

    def comp_part(self, j):
        p = self.g['composite_sprites']['parts'][j]
        return p['child'], p['ox'], p['oy'], p['is_comp']

    def anim_range(self, i):
        r = self.g['anim_sequences']['ranges']
        return r[i] if 0 <= i < len(r) else (0, -1)

    def anim_frame(self, j):
        p = self.g['anim_sequences']['frames'][j]
        return p['child'], p['ox'], p['oy'], p['is_comp']

    def body_range(self, i):
        r = self.g['unit_bodies']['ranges']
        return r[i] if 0 <= i < len(r) else (0, -1)

    def body_layer(self, j):
        p = self.g['unit_bodies']['layers'][j]
        return p['child'], p['ox'], p['oy'], p['is_comp'], p['flag']

    def seq_range(self, i):
        r = self.g['unit_sequences']['ranges']
        return r[i] if 0 <= i < len(r) else (0, -1)

    def seq_step(self, j):
        p = self.g['unit_sequences']['steps'][j]
        return p['body'], p['ox'], p['oy']

    def building(self, i):
        b = self.g['buildings']
        if not (0 <= i < len(b['ranges'])):
            return None
        return (b['ranges'][i][0], b['ranges'][i][1], b['widths'][i], b['heights'][i])

    def building_part(self, j):
        p = self.g['buildings']['parts'][j]
        return p['child'], p['ox'], p['oy'], p['is_comp'], p['layer'], p.get('state', 0)


class New(Lengths):
    def __init__(self, dump, sprite_dir):
        self.j = dump
        self.dir = sprite_dir
        self.images = {}
        self.cache = {}

    def image(self, path):
        if path not in self.images:
            self.images[path] = Image.open(path).convert('RGBA') if os.path.exists(path) else None
        return self.images[path]

    def frame(self, i, faction):
        fr = self.j['frames']
        if not (0 <= i < len(fr)):
            return None
        page, sx, sy, sw, sh, w, h, recolor = fr[i]
        if w <= 0 or h <= 0 or sw <= 0 or sh <= 0:
            return None
        img = self.j['images'][page]
        path = img['resistance'] if faction == RES and img['resistance'] else img['path']
        key = (i, path)
        if key not in self.cache:
            im = self.image(path)
            if im is None:
                self.cache[key] = None
            else:
                c = im.crop((round(sx), round(sy), round(sx) + round(sw), round(sy) + round(sh)))
                self.cache[key] = hashlib.md5(c.tobytes() + bytes(str(c.size), 'ascii')).hexdigest()
        if self.cache[key] is None:
            return None
        return (self.cache[key], w, h, recolor)

    def _r(self, name, i):
        r = self.j[name]
        return r[i] if 0 <= i < len(r) else (0, -1)

    def comp_range(self, i):
        return self._r('composite_ranges', i)

    def comp_part(self, j):
        return tuple(self.j['composite_parts'][j])

    def anim_range(self, i):
        return self._r('anim_ranges', i)

    def anim_frame(self, j):
        return tuple(self.j['anim_frames'][j])

    def body_range(self, i):
        return self._r('body_ranges', i)

    def body_layer(self, j):
        return tuple(self.j['body_layers'][j])

    def seq_range(self, i):
        return self._r('sequence_ranges', i)

    def seq_step(self, j):
        return tuple(self.j['sequence_steps'][j])

    def building(self, i):
        b = self.j['buildings']
        if not (0 <= i < len(b)):
            return None
        return tuple(b[i][:4])

    def building_part(self, j):
        return tuple(self.j['building_parts'][j])


class Sim:
    """Renderer::Draw* replayed: a list of (x, y, pixels, w, h, team)"""

    def __init__(self, t, faction):
        self.t = t
        self.f = faction
        self.out = []

    def raw(self, i, x, y):
        fr = self.t.frame(i, self.f)
        if fr:
            self.out.append((round(x, 3), round(y, 3)) + fr)

    def comp(self, i, x, y, depth=0):
        if depth > 6:
            return
        s, e = self.t.comp_range(i)
        for p in range(s, e + 1):
            child, ox, oy, ic = self.t.comp_part(p)
            if ic == 0:
                self.raw(child, x + ox, y + oy)
            else:
                self.comp(child, x + ox, y + oy, depth + 1)

    def anim(self, i, frame, x, y):
        s, e = self.t.anim_range(i)
        if e < s:
            return
        child, ox, oy, ic = self.t.anim_frame(s + abs(frame) % (e - s + 1))
        if ic == 0:
            self.raw(child, x + ox, y + oy)
        else:
            self.comp(child, x + ox, y + oy)

    def anim_len(self, i):
        s, e = self.t.anim_range(i)
        return e - s + 1 if e >= s else 0

    def seq_len(self, i):
        s, e = self.t.seq_range(i)
        return e - s + 1 if e >= s else 0

    def unit(self, seq_base, arch, action, facing, step, target, death_seq=None, directional=None):
        """DrawUnitWorld minus wrecks; action: idle / move / attack / transform / die. `directional`: the base has
        one sequence per facing (old tables: base < 230; new ones: the family has 8 members)"""
        if action == 'die':
            s, e = self.t.seq_range(death_seq)
            if e < s:
                return
            body, sx, sy = self.t.seq_step(s + min(max(step, 0), e - s))
            bs, be = self.t.body_range(body)
            for b in range(bs, be + 1):
                child, ox, oy, ic, flag = self.t.body_layer(b)
                if ic == 0:
                    self.raw(child, sx + ox, sy + oy)
                elif ic == 1:
                    self.comp(child, sx + ox, sy + oy)
            return
        shoot = 8 if (arch <= 3 and action == 'attack') else 0
        dir8 = 0 if 14 <= arch <= 16 else facing % 8
        tdir8 = target % 8 if target >= 0 else facing % 8
        seq = seq_base
        if directional is None:
            directional = seq_base < 230
        if action != 'transform' and directional:
            seq = seq_base + shoot + dir8
        s, e = self.t.seq_range(seq)
        if e < s:
            return
        count = e - s + 1
        idx = s + (min(max(step, 0), count - 1) if action == 'transform' else abs(step) % count)
        body, sx, sy = self.t.seq_step(idx)
        bs, be = self.t.body_range(body)
        if be < bs:
            return
        s_b53 = [1, 0, 2, 0, 1, 2]
        mask = 0x07 | (0x38 if action == 'attack' else 0)
        for lay in range(bs, be + 1):
            child, ox, oy, ic, flag = self.t.body_layer(lay)
            if flag >= 6 or not (mask & (1 << flag)):
                continue
            if s_b53[flag] > 0:
                if arch == 17:
                    continue
                child += tdir8 if action == 'transform' else (tdir8 - dir8)
            x, y = sx + ox, sy + oy
            if ic == 0:
                self.raw(child, x, y)
            elif ic == 1:
                self.comp(child, x, y)
            elif ic == 2:
                if flag <= 2:
                    self.anim(child, step, x, y)
                else:
                    n = max(1, self.anim_len(child))
                    mask &= ~(1 << [1, 0, 2][flag - 3])
                    self.anim(child, abs(step) % n, x, y)

    def building(self, b, kind_index, layer, frame, progress, hp, tdir, attacking, powered):
        """DrawBuildingWorld for a living building; kind_index is the old building type (its behaviour)"""
        bd = self.t.building(b)
        if not bd or bd[1] < bd[0]:
            return
        start, end = bd[0], bd[1]
        if kind_index >= 15:
            for p in range(start, end + 1):
                child, ox, oy, ic, lay, state = self.t.building_part(p)
                in_layer = layer == 0 if kind_index >= 133 else lay == layer
                if in_layer and state == 0:
                    if ic == 0:
                        self.raw(child, ox, oy)
                    elif ic == 1:
                        self.comp(child, ox, oy)
                    elif ic == 2 and kind_index < 30:
                        self.anim(child, frame, ox, oy)
            return
        under = progress < 1.0
        ia = 4 if under else min(max(7 - int(min(max(hp, 0.0), 1.0) * 4.0), 4), 7)
        tdir8 = tdir % 8
        for p in range(start, end + 1):
            child, ox, oy, ic, lay, state = self.t.building_part(p)
            if lay == 2:
                if layer != 1:
                    continue
                if state == 1:
                    ok = kind_index in (5, 13) or powered
                    if not under and ok and attacking and ic == 2:
                        self.anim(child + tdir8, frame, ox, oy)
                elif state == 2:
                    if ic == 1:
                        self.comp(child + tdir8, ox, oy)
                elif state == 10:
                    needs = kind_index not in (5, 13)
                    if (under or (not powered and needs)) and ic == 1:
                        self.comp(child, ox, oy)
            elif lay == layer:
                match = state == 0 or (layer == 0 and state == ia) or (layer == 1 and 0 < state <= ia)
                if not match:
                    continue
                if ic == 0:
                    self.raw(child, ox, oy)
                elif ic == 1:
                    self.comp(child, ox, oy)
                elif ic == 2 and not under and powered:
                    self.anim(child, frame, ox, oy)


def draw(t, faction, fn):
    s = Sim(t, faction)
    fn(s)
    return s.out


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    assets, conv, dumper = sys.argv[1:]
    d = cs.Data(assets)
    idmap = json.load(open(os.path.join(conv, 'sprite_id_map.json')))
    sprite_dir = os.path.join(conv, 'sprites')

    # Names to resolve: every item the map names and every family of every object
    import yaml
    names = set()
    for k in cs.MAP_KEY.values():
        names.update(idmap[k].values())
    for f in os.listdir(sprite_dir):
        if f.endswith('_meta.yaml'):
            meta = yaml.safe_load(open(os.path.join(sprite_dir, f)))
            for fam in (meta.get('families') or {}):
                names.add('%s.%s' % (f[7:-10], fam))
    with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as fh:
        fh.write('\n'.join(sorted(names)) + '\n')
        names_path = fh.name
    out = subprocess.run([dumper, sprite_dir, names_path], capture_output=True, text=True)
    os.unlink(names_path)
    dump = json.loads(out.stdout)
    if dump['errors']:
        print('registry errors:', *dump['errors'][:20], sep='\n  ')
        return 1
    res = dump['names']

    def h(ref):
        """handle of "<object>.<item>" / family start"""
        return res[ref][1]

    def fam(obj, name):
        return res.get('%s.%s' % (obj, name), [None, -1, 0])

    old, new = Old(d), New(dump, sprite_dir)
    checks = 0
    failures = []

    def same(label, fo, fn):
        nonlocal checks
        for faction in (RES, CONF):
            a, b = draw(old, faction, fo), draw(new, faction, fn)
            checks += 1
            if a != b:
                failures.append((label + ' ' + faction, a[:3], b[:3], len(a), len(b)))
                return

    # Every composite, animation (every frame) and frame on its own, by the item it went to
    for k in ('frames', 'composites', 'anims'):
        for oid, ref in idmap[k].items():
            i, n = int(oid), h(ref)
            if k == 'frames':
                same('frame %d' % i, lambda s, i=i: s.raw(i, 0, 0), lambda s, n=n: s.raw(n, 0, 0))
            elif k == 'composites':
                same('composite %d' % i, lambda s, i=i: s.comp(i, 0, 0), lambda s, n=n: s.comp(n, 0, 0))
            else:
                for fr in range(max(1, old.anim_len(i))):
                    same('anim %d frame %d' % (i, fr), lambda s, i=i, fr=fr: s.anim(i, fr, 0, 0),
                         lambda s, n=n, fr=fr: s.anim(n, fr, 0, 0))

    # Units: every archetype, action, facing, turret facing and step
    arch = d.g['unit_archetypes']
    for a in range(len(arch)):
        info = idmap['archetypes'][str(a)]
        obj, pre = info['object'], info['prefix']
        row = arch[a]
        actions = [('idle', row[0], 'idle'), ('move', row[1], 'move'), ('attack', row[0], 'idle')]
        if len(row) > 3:
            actions.append(('transform', row[3], 'transform'))
        for action, old_base, famname in actions:
            nf = fam(obj, pre + famname)
            if nf[1] < 0:
                failures.append(('unit %d: no family %s%s' % (a, pre, famname), None, None, 0, 0))
                continue
            new_base = nf[1]
            if action == 'attack' and a <= 3:
                # the firing pose lives in its own family; the renderer adds 8 to the idle base
                att = fam(obj, pre + 'attack')
                if att[1] != new_base + 8:
                    failures.append(('unit %d: attack family is not idle + 8' % a, None, None, 0, 0))
            for facing in range(8):
                for target in [-1] + list(range(8)):
                    steps = max(1, old.seq_len(old_base + (0 if action == 'transform' or old_base >= 230 else
                                                           (8 if action == 'attack' and a <= 3 else 0) +
                                                           (0 if 14 <= a <= 16 else facing))))
                    for step in range(steps):
                        same('unit %d %s f%d t%d s%d' % (a, action, facing, target, step),
                             lambda s: s.unit(old_base, a, action, facing, step, target),
                             lambda s: s.unit(new_base, a, action, facing, step, target,
                                              directional=nf[2] >= 8))
    for seq in (247, 248, 259, 260):
        n = h(idmap['sequences'][str(seq)])
        for step in range(old.seq_len(seq)):
            same('death %d s%d' % (seq, step), lambda s: s.unit(0, 0, 'die', 0, step, -1, seq),
                 lambda s: s.unit(0, 0, 'die', 0, step, -1, n))

    # Buildings: every type, layer, damage, build progress, turret facing, attack, power
    nb = len(d.g['buildings']['ranges'])
    for b in range(nb):
        ref = idmap['buildings'].get(str(b))
        if ref is None:
            continue
        n = h(ref)
        for layer in (0, 1):
            for progress in (0.5, 1.0):
                for hp in (1.0, 0.7, 0.4, 0.1):
                    for tdir in range(8 if b < 15 else 1):
                        for attacking in (False, True):
                            for powered in (False, True):
                                for frame in range(3):
                                    same('building %d l%d p%.1f hp%.1f t%d a%d w%d f%d' % (
                                        b, layer, progress, hp, tdir, attacking, powered, frame),
                                        lambda s: s.building(b, b, layer, frame, progress, hp, tdir, attacking,
                                                             powered),
                                        lambda s: s.building(n, b, layer, frame, progress, hp, tdir, attacking,
                                                             powered))
        # the new building's style must give the old type's behaviour
        style = dump['buildings'][n][4]
        # styles: 0 structure (0-14), 1 wall (15-29: army colour, animated), 2 decor (30-132), 3 flat decor (133+)
        want = 0 if b < 15 else 1 if b < 30 else 2 if b < 133 else 3
        if style != want:
            failures.append(('building %d style %d, want %d' % (b, style, want), None, None, 0, 0))

    # Families the game indexes: tiles, decor, wrecks, projectiles, smoke, wall damage
    for t, info in idmap['tilesets'].items():
        lst = d.g['tilesets'][int(t)]
        off = info['decor_offset']
        tiles, decor = fam(info['object'], 'tiles'), fam(info['object'], 'decor')
        for i, c in enumerate(lst[:off]):
            same('tileset %s tile %d' % (t, i), lambda s: s.comp(c, 0, 0), lambda s: s.comp(tiles[1] + i, 0, 0))
        for i, b in enumerate(lst[off:]):
            nbd = decor[1] + i
            for layer in (0, 1):
                same('tileset %s decor %d l%d' % (t, i, layer),
                     lambda s: s.building(b, b, layer, 0, 1.0, 1.0, 0, False, True),
                     lambda s: s.building(nbd, b, layer, 0, 1.0, 1.0, 0, False, True))
    for c, info in idmap['wrecks'].items():
        st = fam(info['object'], 'stages')
        for i in range(info['stages']):
            same('wreck %s stage %d' % (c, i), lambda s: s.comp(int(c) + i, 0, 0),
                 lambda s: s.comp(st[1] + i, 0, 0))
    for p, info in idmap['projectiles'].items():
        dirs = fam(info['object'], 'dirs')
        for i in range(info['directions']):
            same('projectile %s dir %d' % (p, i), lambda s: s.comp(info['base'] + i, 0, 0),
                 lambda s: s.comp(dirs[1] + i, 0, 0))
    smoke = fam('fx_smoke_puff', 'frames')
    for i in range(14):
        same('smoke %d' % i, lambda s: s.comp(1440 + i, 0, 0), lambda s: s.comp(smoke[1] + i, 0, 0))

    print('%d comparisons, %d failures' % (checks, len(failures)))
    for f in failures[:25]:
        print('  FAIL', f[0], '| old', f[3], 'draws', f[1], '| new', f[4], 'draws', f[2])
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
