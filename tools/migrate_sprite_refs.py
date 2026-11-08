#!/usr/bin/env python3
"""Rewrites the sprite numbers of the configs into sprite names, after tools/convert_sprites.py.

    tools/migrate_sprite_refs.py <assets dir> <sprite_id_map.json>

Edits in place, line by line (comments and layout stay):
  characters/<unit>.yaml      sprite: <object> (+ sprite_set / baseline_sprite_set of the other forms),
                              production.icon_composite, animations.death_sequences / flame_death_sequences /
                              wreck_composite
  characters/<building>.yaml  sprite: <object>.<building> (walls: the damage family, + reinforced_sprite),
                              production.icon_composite, animations.wreck_composite
  characters/research_*.yaml  icon
  characters/effects.yaml     explosion animations, wrecks (composite -> sprite: <object>.stages)
  characters/projectiles.yaml sprite (-> <object>.dirs), impact, impact_wall, wall_destroyed, ground_marks
"""
import json
import os
import re
import sys

NUM_LIST = re.compile(r'^(\s*)([a-z_]+):\s*\[([0-9,\s-]*)\](.*)$')
NUM = re.compile(r'^(\s*)([a-z_]+):\s*(-?\d+)(\s*(#.*)?)$')
OLD_COMMENT = re.compile(r'Animation ids are animations of graphics_data\.json, composite ids are its composites\.')
NEW_COMMENT = 'Animations and composites are sprites by name: <object>.<item> (assets/sprites/sprite_<object>_meta.yaml).'


class Migrator:
    def __init__(self, idmap):
        self.m = idmap
        self.problems = []

    def name(self, kind, i, where):
        v = self.m[kind].get(str(i))
        if v is None:
            self.problems.append('%s: no %s %s' % (where, kind, i))
            return None
        return v

    def names(self, kind, values, where):
        out = []
        for v in values.split(','):
            v = v.strip()
            if v:
                n = self.name(kind, int(v), where)
                if n:
                    out.append(n)
        return '[' + ', '.join(out) + ']'

    def wreck(self, comp, where):
        w = self.m['wrecks'].get(str(comp))
        if w:
            return w['object'] + '.stages'
        return self.name('composites', comp, where)


def migrate_character(mg, path, lines):
    text = ''.join(lines)
    unit = re.search(r'^archetype_id:\s*(\d+)', text, re.M)
    building = re.search(r'^building_index:\s*(\d+)', text, re.M)
    out = []
    in_production = False
    for line in lines:
        out.append(line)
        if re.match(r'^\S', line):
            in_production = line.startswith('production:')
        m = NUM.match(line.rstrip('\n'))
        ml = NUM_LIST.match(line.rstrip('\n'))
        if unit and re.match(r'^archetype_id:', line):
            a = unit.group(1)
            out.append('sprite: %s\n' % mg.m['archetypes'][a]['object'])
        elif unit and m and m.group(2) == 'archetype_id' and m.group(1):
            pre = mg.m['archetypes'].get(m.group(3), {}).get('prefix', '')
            if pre:
                out.append('%ssprite_set: %s\n' % (m.group(1), pre.rstrip('_')))
        elif unit and m and m.group(2) == 'baseline_archetype_id':
            pre = mg.m['archetypes'].get(m.group(3), {}).get('prefix', '')
            if pre:
                out.append('%sbaseline_sprite_set: %s\n' % (m.group(1), pre.rstrip('_')))
        elif unit and line.startswith('production:'):
            cid = re.search(r'^id:\s*"?([a-z0-9_]+)', text, re.M).group(1)
            fac = 'res' if cid.endswith('_res') else 'conf'
            icon = 'icons_%s.%s' % (fac, re.sub(r'_(res|conf)$', '', cid))
            if icon in mg.m['composites'].values() and 'icon_composite' not in text:
                out.append('  icon_composite: %s\n' % icon)
        elif building and re.match(r'^building_index:', line):
            b = int(building.group(1))
            ref = mg.m['building_index'][str(b)]
            if b >= 15:
                ref = ref.rsplit('.', 1)[0] + '.damage'
            out.append('# Sprite of the building (walls: the family of damage stages)\n')
            out.append('sprite: %s\n' % ref)
            if b == 15:
                out.append('# Walls after the wall research\n')
                out.append('reinforced_sprite: %s.damage\n' % mg.m['building_index']['20'].rsplit('.', 1)[0])
        elif m and m.group(2) == 'icon_composite':
            out[-1] = '%sicon_composite: %s%s\n' % (m.group(1), mg.name('composites', int(m.group(3)), path),
                                                    m.group(4) or '')
        elif m and m.group(2) == 'wreck_composite':
            out[-1] = '%swreck_composite: %s%s\n' % (m.group(1), mg.wreck(int(m.group(3)), path), m.group(4) or '')
        elif ml and ml.group(2) in ('death_sequences', 'flame_death_sequences', 'death_variants',
                                    'flame_death_variants'):
            out[-1] = '%s%s: %s%s\n' % (ml.group(1), ml.group(2), mg.names('sequences', ml.group(3), path),
                                        ml.group(4))
    return out


def migrate_research(mg, path, lines):
    out = []
    for line in lines:
        m = NUM.match(line.rstrip('\n'))
        if m and m.group(2) == 'icon':
            line = '%sicon: %s%s\n' % (m.group(1), mg.name('composites', int(m.group(3)), path), m.group(4) or '')
        out.append(line)
    return out


def migrate_effects(mg, path, lines):
    out = []
    section = None
    for line in lines:
        if re.match(r'^\S', line):
            section = line.split(':')[0]
        m = NUM.match(line.rstrip('\n'))
        ml = NUM_LIST.match(line.rstrip('\n'))
        if section == 'explosions' and ml and ml.group(2) in ('main', 'second', 'extra_anims'):
            line = '%s%s: %s%s\n' % (ml.group(1), ml.group(2), mg.names('anims', ml.group(3), path), ml.group(4))
        elif section == 'explosions' and m and m.group(2) in ('small', 'power', 'large', 'big_anim'):
            line = '%s%s: %s%s\n' % (m.group(1), m.group(2), mg.name('anims', int(m.group(3)), path),
                                     m.group(4) or '')
        elif section == 'wrecks' and m and m.group(2) == 'composite':
            line = '%ssprite: %s%s\n' % (m.group(1), mg.wreck(int(m.group(3)), path), m.group(4) or '')
        line = line.replace('# Vehicles: first composite, number of stages (composite + stage is drawn)',
                            '# Vehicles: the family of wreck stages (sprite) and how many of them are drawn')
        out.append(line)
    return out


def migrate_projectiles(mg, path, lines):
    by_base = {str(p['base']): p['object'] for p in mg.m['projectiles'].values()}
    out = []
    for line in lines:
        m = NUM.match(line.rstrip('\n'))
        ml = NUM_LIST.match(line.rstrip('\n'))
        if m and m.group(2) == 'sprite':
            base = m.group(3)
            ref = 'none' if base == '0' else (by_base.get(base, '') + '.dirs' if base in by_base else
                                               mg.name('composites', int(base), path))
            line = '%ssprite: %s%s\n' % (m.group(1), ref, m.group(4) or '')
        elif m and m.group(2) in ('impact_wall', 'wall_destroyed'):
            line = '%s%s: %s%s\n' % (m.group(1), m.group(2), mg.name('anims', int(m.group(3)), path),
                                     m.group(4) or '')
        elif ml and ml.group(2) == 'impact':
            line = '%simpact: %s%s\n' % (ml.group(1), mg.names('anims', ml.group(3), path), ml.group(4))
        elif ml and ml.group(2) == 'ground_marks':
            line = '%sground_marks: %s%s\n' % (ml.group(1), mg.names('composites', ml.group(3), path), ml.group(4))
        line = line.replace('# First composite of the flying projectile, one composite per direction',
                            '# The flying projectile: the family of its directions (none: not drawn)')
        line = line.replace('# The jet is drawn by its own effect (animation 59): no projectile sprite',
                            '# The jet is drawn by its own effect (fx_flame_jet): no projectile sprite')
        out.append(line)
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    assets, mapfile = sys.argv[1:]
    mg = Migrator(json.load(open(mapfile)))
    chars = os.path.join(assets, 'characters')
    for f in sorted(os.listdir(chars)):
        if not f.endswith('.yaml'):
            continue
        path = os.path.join(chars, f)
        lines = open(path).readlines()
        if f.startswith('research_'):
            new = migrate_research(mg, f, lines)
        elif f == 'effects.yaml':
            new = migrate_effects(mg, f, lines)
        elif f == 'projectiles.yaml':
            new = migrate_projectiles(mg, f, lines)
        elif '_unit_' in f or '_build_' in f:
            new = migrate_character(mg, f, lines)
        else:
            continue
        new = [OLD_COMMENT.sub(NEW_COMMENT, line) for line in new]
        if new != lines:
            open(path, 'w').write(''.join(new))
            print('migrated', f)
    for p in mg.problems:
        print('problem:', p)
    return 1 if mg.problems else 0


if __name__ == '__main__':
    sys.exit(main())
