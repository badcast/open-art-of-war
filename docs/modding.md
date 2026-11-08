# Mods

A mod changes or adds game data: sprites, units and buildings, balance, maps, sounds, texts. Mods hold data only, never code.

## Where mods live

- `<the game's folder>/mods/` (on Windows: next to the `.exe`)
- `<the user directory>/mods/` (Linux `~/.local/share/badcast/openaow/mods`, Windows `%APPDATA%\badcast\openaow\mods`)

A mod is either:

- an archive `<name>.mod`: a tar compressed with zstd, the same format as `data.blob`;
- or a folder `<name>/` with the same contents. A folder is handy while you make the mod, because a change shows up without packing it again.

Two mods with the same `id`: the one with the higher `version` is used. On the same version the copy in the user directory wins.

## Contents

The root of a mod holds `mod.yaml` and files laid out like the game's `assets/` folder:

```
my_mod/
  mod.yaml
  sprites/sprite_heavy_tank_res.png        <- replaces the game's sprite
  sprites/sprite_heavy_tank_res_meta.yaml
  characters/resistance_unit_heavy_tank.yaml
  maps/my_map.json                         <- a new file
```

A file with the same path as a game file replaces it. A new file is added. New sprites in `sprites/` are found on their own, like the game's.

## mod.yaml

```yaml
id: better_tanks          # lower case letters, digits and _; the same mod keeps the same id
name: "Better tanks"
version: "1.2.0"          # numbers compared as numbers: 1.10 > 1.9
author: "Your name"
description: "Heavier armour for the tanks of both sides."
game_version: "0.9"       # the game version it was made for (shown to the player)
manifest:                 # added to assets/main.yaml
  maps:
    skirmish: ["maps/my_map.json"]
  net_hash: ["characters/resistance_unit_heavy_tank.yaml"]
```

The `manifest:` section is merged into `main.yaml`:

- mappings are merged key by key;
- lists get the entries they do not have yet;
- any other value is replaced.

## Turning mods on

Main menu → **Mods**:

- the box next to a mod turns it on or off (Space does the same);
- **Up** / **Down** (or Shift+arrows) change the order of the enabled mods. A mod lower in the list wins over the ones above it when two mods bring the same file. The conflicts are listed after Apply;
- **Apply** puts the choice in place and saves it.

Data, sprites and the manifest are read again at once. Music, sounds, fonts and translations change with the next start.

## Lookup order

When the game reads a data file, the first one found counts:

1. the data of a network game or a replay (the host's data, mods included);
2. the enabled mods, the last one first;
3. the installed game data (`assets/` or `data.blob`, plus the user's saved files).

## Network games and replays

A network game plays the host's simulation data (the files of `net_hash` in `main.yaml`, with the host's mods in them). Players without the mod receive that data for the match. A mod that adds simulation files must list them in `net_hash` of its `manifest:` section. Sprites and sounds are not sent, so the other players see their own.

Replays keep the simulation data they were played with and list the mods of the recording computer.

## Packing

```
python3 tools/pack_assets.py --mod my_mod/ my_mod.mod [assets/]
```

This checks `mod.yaml` (an id and a version) and that every file named in `manifest:` is in the mod. Give the game's `assets/` as the last argument to also accept the files the game already has. Paths that would leave the mod (`..`, absolute ones) are refused.
