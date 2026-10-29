#!/usr/bin/env python3
"""Packs the contents of the assets directory into data.blob (a tar archive compressed with zstd).

Entries are stored relative to the assets directory ("maps/m0.json", "locale/en/...").
The game reads the blob with libarchive (see src/core/AssetStore.cpp).
Usage: pack_assets.py <assets_dir> <output.blob>
"""
import io
import os
import shutil
import subprocess
import sys
import tarfile

FIXED_TIME = 1767225600  # 2026-01-01, reproducible archives
ZSTD_LEVEL = 19


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


def main():
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
            files.append((os.path.relpath(path, assets_dir).replace(os.sep, "/"), path))

    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for key, path in files:
            info = tarfile.TarInfo(key)
            info.size = os.path.getsize(path)
            info.mtime = FIXED_TIME
            info.mode = 0o644
            with open(path, "rb") as f:
                tar.addfile(info, f)
    tmp = output + ".tmp"
    compress(buf.getvalue(), tmp)
    os.replace(tmp, output)
    print(f"pack_assets: {len(files)} files -> {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
