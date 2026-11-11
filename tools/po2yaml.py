#!/usr/bin/env python3
"""Converts GNU gettext .po files to Open Art of War YAML localization catalogs.
English strings reside directly in the source code; only translated languages (e.g. ru)
are exported to assets/locale/<lang>/openaow.yaml.
Usage: python3 tools/po2yaml.py [po_dir] [assets_locale_dir]
"""

import glob
import json
import os
import sys


def parse_po(path):
    entries = {}
    with open(path, "r", encoding="utf-8") as fp:
        lines = fp.read().splitlines()
    i = 0
    cur_id = None
    cur_str = None
    state = None

    while i < len(lines):
        line = lines[i].strip()
        if not line or line.startswith("#"):
            i += 1
            continue
        if line.startswith("msgid "):
            if cur_id is not None and cur_str is not None:
                if cur_id:  # skip header
                    entries[cur_id] = cur_str
            raw = line[6:].strip()
            cur_id = eval(raw)
            cur_str = ""
            state = "msgid"
        elif line.startswith("msgstr "):
            raw = line[7:].strip()
            cur_str = eval(raw)
            state = "msgstr"
        elif line.startswith('"') and line.endswith('"'):
            val = eval(line)
            if state == "msgid":
                cur_id += val
            elif state == "msgstr":
                cur_str += val
        i += 1

    if cur_id is not None and cur_str is not None and cur_id:
        entries[cur_id] = cur_str
    return entries


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    po_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "po")
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "assets", "locale")

    po_files = sorted(glob.glob(os.path.join(po_dir, "*.po")))
    if not po_files:
        print(f"No .po files found in {po_dir}")
        return 1

    for po_file in po_files:
        base = os.path.basename(po_file)
        name, _ = os.path.splitext(base)
        parts = name.rsplit("_", 1)
        if len(parts) == 2 and parts[1] in ("en", "ru"):
            domain, lang = parts[0], parts[1]
        else:
            domain, lang = name, "ru"

        # English is native in code: do not generate en catalog
        if lang == "en":
            continue

        # Write to assets/locale/<lang>/<domain>.yaml
        target_dir = os.path.join(out_dir, lang)
        os.makedirs(target_dir, exist_ok=True)
        yaml_file = os.path.join(target_dir, f"{domain}.yaml")

        entries = parse_po(po_file)
        with open(yaml_file, "w", encoding="utf-8") as out:
            out.write(f"# Open Art Of War localization catalog ({lang}): {domain}\n")
            for k, v in entries.items():
                out.write(f"{json.dumps(k, ensure_ascii=False)}: {json.dumps(v, ensure_ascii=False)}\n")
        print(f"Converted {len(entries)} strings: {base} -> {yaml_file}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
