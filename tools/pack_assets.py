#!/usr/bin/env python3
"""Packs the contents of the assets directory into a blob (a tar archive compressed with zstd) together with its
integrity certificate.

Entries are stored relative to the assets directory ("maps/*.json", "locale/en/...").
The game reads the blob with libarchive (see src/core/AssetStore.cpp).

The certificate is the entry "certificate.sha256" inside the blob itself (there is no separate file): every packed
file, one per line, "<path>  <sha256 in hex>", the path as the game names it ("assets/maps/m0.json"). When the game
loads a blob it checks the blob's files against that certificate and refuses to run when the certificate is missing
or a file is missing or differs (Assets::VerifyCertificate).

Usage: pack_assets.py <assets_dir> <output.blob>
       pack_assets.py --mod <mod_dir> <output.mod> [<assets_dir> to check the names of mod.yaml against]
"""
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import posixpath
import re

try:
    import yaml  # optional dependency for the main manifest and mod manifests
except ModuleNotFoundError:  # pragma: no cover - fallback for minimal environments
    yaml = None

FIXED_TIME = 1767225600  # 2026-01-01, reproducible archives
ZSTD_LEVEL = 19
CERTIFICATE_NAME = "certificate.sha256"


def strip_yaml_comment(line):
    """Strip comments outside of quoted strings, as YAML uses # for comments."""
    out = []
    in_single = False
    in_double = False
    escape = False
    for ch in line:
        if ch == "\\" and (in_double and not escape):
            escape = True
            out.append(ch)
            continue
        if ch == "'" and not in_double:
            in_single = not in_single
        elif ch == '"' and not in_single:
            in_double = not in_double
        elif ch == "#" and not in_single and not in_double:
            break
        out.append(ch)
        escape = False
    return "".join(out).rstrip()


def parse_scalar(value):
    value = value.strip()
    if value == "":
        return ""
    if value in ("null", "Null", "NULL", "~"):
        return None
    lower = value.lower()
    if lower == "true":
        return True
    if lower == "false":
        return False
    if value and value[0] in ('"', "'") and value[-1] == value[0]:
        inner = value[1:-1]
        return inner.encode("utf-8").decode("unicode_escape").replace("\\'", "'")
    if re.fullmatch(r"-?\d+", value):
        return int(value)
    if re.fullmatch(r"-?\d+\.\d+([eE][-+]?\d+)?", value):
        return float(value)
    if value.startswith("[") and value.endswith("]"):
        return [item.strip() for item in value[1:-1].split(",") if item.strip()]
    return value


def yaml_load_text(text):
    if yaml is not None:
        return yaml.safe_load(text)

    lines = []
    for raw in text.splitlines():
        stripped = strip_yaml_comment(raw)
        if not stripped.strip():
            continue
        indent = len(stripped) - len(stripped.lstrip(" "))
        lines.append((indent, stripped.strip()))

    def parse_block(index, indent):
        if index >= len(lines):
            return {}, index
        current_indent, first = lines[index]
        if current_indent < indent:
            return {}, index
        if first.startswith("- "):
            items = []
            while index < len(lines):
                cur_indent, content = lines[index]
                if cur_indent < indent:
                    break
                if cur_indent != indent or not content.startswith("- "):
                    break
                rest = content[2:].strip()
                index += 1
                if not rest:
                    value, index = parse_block(index, indent + 2)
                    items.append(value)
                    continue
                if ":" in rest:
                    key, value_part = rest.split(":", 1)
                    key = key.strip()
                    value_part = value_part.strip()
                    item = {key: parse_scalar(value_part) if value_part else None}
                    if not value_part:
                        nested, index = parse_block(index, indent + 2)
                        item[key] = nested
                    while index < len(lines) and lines[index][0] > indent:
                        more, index = parse_block(index, indent + 2)
                        if isinstance(more, dict):
                            item.update(more)
                        else:
                            item[key] = more
                            break
                    items.append(item)
                    continue
                items.append(parse_scalar(rest))
            return items, index

        mapping = {}
        while index < len(lines):
            cur_indent, content = lines[index]
            if cur_indent < indent:
                break
            if cur_indent != indent or content.startswith("- "):
                break
            if ":" not in content:
                raise ValueError(f"Unsupported YAML line: {content!r}")
            key, value_part = content.split(":", 1)
            key = key.strip()
            value_part = value_part.strip()
            index += 1
            if value_part:
                mapping[key] = parse_scalar(value_part)
                continue
            # nested block
            child, index = parse_block(index, indent + 2)
            mapping[key] = child
        return mapping, index

    result, _ = parse_block(0, 0)
    return result


def yaml_load_file(path):
    with open(path, "r", encoding="utf-8") as fh:
        return yaml_load_text(fh.read())


def compress(tar_bytes, output):
    try:
        from compression import zstd  # Python 3.14+
        with open(output, "wb") as out:
            out.write(zstd.compress(tar_bytes, level=ZSTD_LEVEL))
        return
    except ImportError:
        pass
    exe = shutil.which("zstd")
    if exe is None:
        raise SystemExit("pack_assets: Python 3.14 (compression.zstd) or the zstd program is required")
    with open(output, "wb") as out:
        subprocess.run([exe, f"-{ZSTD_LEVEL}", "-q", "-c"], input=tar_bytes, stdout=out, check=True)


def certificate_text(hashes):
    """hashes: [(key, sha256 hex)] in packing order"""
    lines = ["# Open Art Of War: SHA-256 of every file of this blob, written by tools/pack_assets.py",
             "# <path>  <sha256>"]
    lines += [f"assets/{key}  {digest}" for key, digest in hashes]
    return ("\n".join(lines) + "\n").encode("utf-8")


def add_entry(tar, key, data):
    info = tarfile.TarInfo(key)
    info.size = len(data)
    info.mtime = FIXED_TIME
    info.mode = 0o644
    tar.addfile(info, io.BytesIO(data))


def manifest_paths(node, key=""):
    """Every file path named in main.yaml (the keys that hold names, not files, are skipped)"""
    if key in ("_comment", "version", "default_language", "fallback_language", "dir") or key.endswith("_dir"):
        return
    if isinstance(node, str):
        yield node
    elif isinstance(node, list):
        for item in node:
            yield from manifest_paths(item)
    elif isinstance(node, dict):
        for k, v in node.items():
            yield from manifest_paths(v, k)


def check_manifest(assets_dir, keys):
    """The manifest must name only files included in the archive inventory."""
    manifest_yaml = os.path.join(assets_dir, "main.yaml")
    if not os.path.isfile(manifest_yaml):
        raise SystemExit(f"pack_assets: main.yaml not found in {assets_dir}")
    data = yaml_load_file(manifest_yaml)
    manifest_name = "main.yaml"

    named = set(manifest_paths(data))
    # Check the actual archive inventory: an existing file may have been skipped
    # (for example a hidden file or the generated certificate).
    missing = sorted(named - set(keys))
    if missing:
        raise SystemExit(f"pack_assets: {manifest_name} names missing files:\n  " + "\n  ".join(missing))
    return len(named)


MOD_ID = re.compile(r"^[a-z0-9_]+$")


def mod_key(name):
    """A path inside a mod as the game reads it (below assets/); None when it would leave the mod"""
    if not name or "\\" in name or ":" in name or name.startswith("/"):
        return None
    key = posixpath.normpath(name)
    if key in (".", "..") or key.startswith("../"):
        return None
    if key.startswith("assets/"):
        key = key[len("assets/"):]
    return key or None


def check_mod(mod_dir, keys, base_assets=None):
    """mod.yaml must name the mod; every path its manifest section names must be in the mod or the base game"""
    path = os.path.join(mod_dir, "mod.yaml")
    if not os.path.isfile(path):
        raise SystemExit(f"pack_assets: {mod_dir} has no mod.yaml")
    data = yaml_load_file(path)
    if not isinstance(data, dict):
        raise SystemExit("pack_assets: mod.yaml is not a mapping")
    if not MOD_ID.match(str(data.get("id", ""))):
        raise SystemExit('pack_assets: mod.yaml needs an "id" of lower case letters, digits and _')
    if "version" not in data:
        raise SystemExit('pack_assets: mod.yaml needs a "version"')
    manifest = data.get("manifest") or {}
    if not isinstance(manifest, dict):
        raise SystemExit('pack_assets: "manifest" in mod.yaml must be a mapping')
    have = set(keys)
    missing = []
    for named in manifest_paths(manifest):
        key = mod_key(named)
        if key is None:
            raise SystemExit(f"pack_assets: mod.yaml names {named}, which is outside the game data")
        if key in have:
            continue
        if base_assets and os.path.isfile(os.path.join(base_assets, key)):
            continue
        missing.append(named)
    if missing:
        raise SystemExit("pack_assets: mod.yaml names missing files:\n  " + "\n  ".join(sorted(missing)))
    return data


def pack_mod(mod_dir, output, base_assets=None):
    """A mod folder as <name>.mod: a tar.zst with mod.yaml and the files laid out as below assets/"""
    files = []
    for root, dirs, names in os.walk(mod_dir):
        dirs.sort()
        for name in sorted(names):
            if name.startswith(".") or name.endswith(".bak") or name.startswith("~"):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, mod_dir).replace(os.sep, "/")
            key = mod_key(rel)
            if key is None:
                raise SystemExit(f"pack_assets: {rel} is outside the mod")
            files.append((key, path))
    data = check_mod(mod_dir, [k for k, _ in files if k != "mod.yaml"], base_assets)
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for key, path in files:
            with open(path, "rb") as f:
                add_entry(tar, key, f.read())
    tmp = output + ".tmp"
    compress(buf.getvalue(), tmp)
    os.replace(tmp, output)
    print(f"pack_assets: mod {data['id']} {data['version']}: {len(files)} files -> {output}")
    return 0


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--mod":
        if len(sys.argv) not in (4, 5):
            print(__doc__)
            return 2
        mod_dir, output = sys.argv[2], sys.argv[3]
        base = sys.argv[4] if len(sys.argv) == 5 else None
        if not os.path.isdir(mod_dir):
            print(f"pack_assets: {mod_dir} is not a directory", file=sys.stderr)
            return 1
        return pack_mod(mod_dir, output, base)
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    assets_dir, output = sys.argv[1], sys.argv[2]
    if not os.path.isdir(assets_dir):
        print(f"pack_assets: {assets_dir} is not a directory", file=sys.stderr)
        return 1

    files = []
    for root, dirs, names in os.walk(assets_dir):
        dirs.sort()
        for name in sorted(names):
            if name.startswith("."):
                continue
            path = os.path.join(root, name)
            key = os.path.relpath(path, assets_dir).replace(os.sep, "/")
            if key == CERTIFICATE_NAME:
                continue  # the blob's own certificate is written below, never taken from the folder
            files.append((key, path))

    named = check_manifest(assets_dir, [k for k, _ in files])

    hashes = []
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for key, path in files:
            with open(path, "rb") as f:
                data = f.read()
            hashes.append((key, hashlib.sha256(data).hexdigest()))
            add_entry(tar, key, data)
        add_entry(tar, CERTIFICATE_NAME, certificate_text(hashes))
    tmp = output + ".tmp"
    compress(buf.getvalue(), tmp)
    os.replace(tmp, output)
    print(f"pack_assets: {len(files)} files ({named} named in main.yaml) + {CERTIFICATE_NAME} -> {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
