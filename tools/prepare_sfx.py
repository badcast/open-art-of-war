#!/usr/bin/env python3
"""Builds the game's sound effects from tools/sfx_sources.json.

Every sound comes from a CC0 / public-domain source (Kenney packs through their GitHub mirrors, Wikimedia
Commons, voice lines generated with espeak-ng). The script downloads the sources into a cache, checks
their licence and checksum, cuts and processes every variant (pitch, bursts, radio voice, echo, fades,
peak normalisation) and writes mono 44.1 kHz Ogg Vorbis files:

    assets/audio/sfx/<event>_<n>.ogg     (the game groups the variants of an event by that name)
    assets/audio/sfx/CREDITS.txt         (source, author and licence of every file)

    tools/prepare_sfx.py [--cache DIR] [--out DIR] [--only EVENT[,EVENT]] [--check]

--voices builds the voice lines instead (unit replies, announcer) from tools/voice_sources.json, one folder per
language: assets/audio/voices/<language>/<event>_<n>.ogg and its CREDITS.txt (--out is then the voices folder).

--check only verifies the output (every event has files, every file is credited, peaks <= -1 dBFS).
Needs: ffmpeg (with libvorbis), espeak-ng, curl, numpy.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.parse

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SR = 44100
ALLOWED_LICENSES = ("cc0", "public domain")
PEAK_DBFS = -1.0          # the check: no file may peak above this
NORMALISE_DBFS = -2.0     # target peak before encoding (Vorbis overshoots a little)


def run(cmd, data=None):
    res = subprocess.run(cmd, input=data, capture_output=True)
    if res.returncode != 0:
        raise RuntimeError(f"{cmd[0]} failed: {res.stderr.decode(errors='replace')[-400:]}")
    return res.stdout


def curl(url, out=None):
    cmd = ["curl", "-sSfL", "--retry", "3", "--retry-delay", "2", "-m", "120", url]
    if out:
        cmd += ["-o", out]
    return run(cmd)


def sha1_of(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


# ---------------------------------------------------------------- sources

def fetch_sources(manifest, cache, needed):
    """Downloads what the recipes use; returns {source id: local dir or file}"""
    os.makedirs(cache, exist_ok=True)
    sources = manifest["sources"]
    local = {}
    # GitHub mirrors: single files at a pinned commit
    for sid, files in needed["github"].items():
        s = sources[sid]
        base = os.path.join(cache, sid)
        os.makedirs(base, exist_ok=True)
        lic_path = os.path.join(base, "LICENSE")
        if not os.path.exists(lic_path):
            for name in ("LICENSE", "LICENSE.txt", s["dir"] + "/LICENSE.txt"):
                try:
                    curl(f"https://raw.githubusercontent.com/{s['repo']}/{s['ref']}/{urllib.parse.quote(name)}", lic_path)
                    break
                except RuntimeError:
                    continue
        text = open(lic_path, encoding="utf-8", errors="replace").read().lower() if os.path.exists(lic_path) else ""
        if "cc0" not in text and "creative commons zero" not in text:
            sys.exit(f"{sid}: no CC0 licence found in {s['repo']}")
        for f in sorted(files):
            dst = os.path.join(base, f)
            if os.path.exists(dst):
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            url = f"https://raw.githubusercontent.com/{s['repo']}/{s['ref']}/{urllib.parse.quote(s['dir'] + '/' + f)}"
            curl(url, dst)
        local[sid] = base
    # Wikimedia Commons: one metadata request for all titles, licence and SHA-1 checked
    titles = {sources[sid]["title"]: sid for sid in needed["commons"]}
    missing = [t for t, sid in titles.items() if not os.path.exists(os.path.join(cache, "commons", t)) or sha1_of(os.path.join(cache, "commons", t)) != sources[sid]["sha1"]]
    os.makedirs(os.path.join(cache, "commons"), exist_ok=True)
    if titles:
        q = urllib.parse.urlencode({"action": "query", "format": "json", "prop": "imageinfo", "iiprop": "url|sha1|extmetadata",
                                    "iiextmetadatafilter": "LicenseShortName", "titles": "|".join("File:" + t for t in titles)})
        meta = json.loads(curl("https://commons.wikimedia.org/w/api.php?" + q))
        info = {}
        for page in meta["query"]["pages"].values():
            ii = page["imageinfo"][0]
            info[page["title"].split(":", 1)[1]] = ii
        for t, sid in titles.items():
            ii = info.get(t) or info.get(t.replace("_", " "))
            if not ii:
                sys.exit(f"{sid}: '{t}' not found on Wikimedia Commons")
            lic = ii["extmetadata"]["LicenseShortName"]["value"]
            if not any(a in lic.lower() for a in ALLOWED_LICENSES):
                sys.exit(f"{sid}: licence '{lic}' is not CC0 / public domain")
            if ii["sha1"] != sources[sid]["sha1"]:
                sys.exit(f"{sid}: the file on Commons changed (sha1 {ii['sha1']}); check it and update the manifest")
            dst = os.path.join(cache, "commons", t)
            if t in missing:
                curl(ii["url"], dst)
                time.sleep(1.0)  # be gentle with upload.wikimedia.org
            if sha1_of(dst) != sources[sid]["sha1"]:
                sys.exit(f"{sid}: checksum mismatch after download")
            local[sid] = dst
    return local


# ---------------------------------------------------------------- audio helpers

def decode(path, start=None, dur=None):
    cmd = ["ffmpeg", "-v", "error"]
    if start:
        cmd += ["-ss", f"{start:.3f}"]
    if dur:
        cmd += ["-t", f"{dur:.3f}"]
    cmd += ["-i", path, "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"]
    return np.frombuffer(run(cmd), dtype=np.float32).copy()


def ffilter(x, af):
    out = run(["ffmpeg", "-v", "error", "-f", "f32le", "-ar", str(SR), "-ac", "1", "-i", "-", "-af", af, "-f", "f32le", "-ar", str(SR), "-ac", "1", "-"],
              x.astype(np.float32).tobytes())
    return np.frombuffer(out, dtype=np.float32).copy()


def pitch(x, semitones):
    if not semitones:
        return x
    ratio = 2.0 ** (semitones / 12.0)
    n = max(1, int(len(x) / ratio))
    return np.interp(np.arange(n) * ratio, np.arange(len(x)), x).astype(np.float32)


def tts(text, cache, voice="en-us+m3"):
    wav = os.path.join(cache, "tts", hashlib.sha1((voice + "|" + text).encode()).hexdigest() + ".wav")
    if voice == "en-us+m3":  # the name the English lines were cached under before voices could be chosen
        old = os.path.join(cache, "tts", hashlib.sha1(text.encode()).hexdigest() + ".wav")
        if os.path.exists(old):
            wav = old
    if not os.path.exists(wav):
        os.makedirs(os.path.dirname(wav), exist_ok=True)
        # A low, measured male voice: the announcer of a command post
        run(["espeak-ng", "-v", voice, "-s", "150", "-p", "32", "-a", "180", "-w", wav, text])
    return decode(wav)


def radio(x, seed):
    """Field radio: telephone band, compressed and a little overdriven, a squelch click at both ends"""
    y = ffilter(x, "highpass=f=320,lowpass=f=3300,acompressor=threshold=-20dB:ratio=6:attack=5:release=60,volume=4dB,asoftclip=type=tanh")
    rng = np.random.default_rng(seed)
    click = (rng.standard_normal(int(0.035 * SR)) * 0.12).astype(np.float32)
    click *= np.linspace(1.0, 0.0, len(click), dtype=np.float32)
    click = ffilter(click, "highpass=f=1200,lowpass=f=5000")
    gap = np.zeros(int(0.04 * SR), dtype=np.float32)
    return np.concatenate([click, gap, y, gap, click * 0.7])


def trim_silence(x, threshold_db=-50.0):
    a = np.abs(x)
    thr = 10 ** (threshold_db / 20.0) * max(a.max(), 1e-9)
    idx = np.where(a > thr)[0]
    if len(idx) == 0:
        return x
    return x[max(0, idx[0] - int(0.002 * SR)): idx[-1] + int(0.01 * SR)]


def fades(x, fade_in=0.003, fade_out=0.02):
    x = x.copy()
    n_in = min(len(x), int(fade_in * SR))
    n_out = min(len(x), int(fade_out * SR))
    if n_in:
        x[:n_in] *= np.linspace(0.0, 1.0, n_in, dtype=np.float32)
    if n_out:
        x[-n_out:] *= np.linspace(1.0, 0.0, n_out, dtype=np.float32) ** 2
    return x


def normalise(x, gain_db=0.0):
    peak = np.abs(x).max()
    if peak <= 1e-9:
        return x
    target = 10 ** ((NORMALISE_DBFS + min(0.0, gain_db)) / 20.0)
    return (x * (target / peak)).astype(np.float32)


def encode(x, out):
    run(["ffmpeg", "-v", "error", "-y", "-f", "f32le", "-ar", str(SR), "-ac", "1", "-i", "-", "-c:a", "libvorbis", "-q:a", "4", out],
        np.clip(x, -1.0, 1.0).astype(np.float32).tobytes())


# ---------------------------------------------------------------- recipes

def render(recipe, local, sources, cache, seed):
    src = recipe["src"]
    if sources[src]["type"] == "tts":
        x = tts(recipe["text"], cache, recipe.get("voice", "en-us+m3"))
    else:
        path = os.path.join(local[src], recipe["file"]) if "file" in recipe else local[src]
        x = decode(path, recipe.get("start"), recipe.get("dur"))
    x = trim_silence(x)
    x = pitch(x, recipe.get("pitch", 0.0))
    if "burst" in recipe:
        b = recipe["burst"]
        step = int(b["interval"] * SR)
        shots = [pitch(x, b.get("pitch_step", 0.0) * i) * (1.0 - 0.06 * i) for i in range(b["count"])]
        out = np.zeros(step * (b["count"] - 1) + max(len(s) for s in shots), dtype=np.float32)
        for i, s in enumerate(shots):
            out[i * step: i * step + len(s)] += s
        x = out
    if recipe.get("filter"):
        x = ffilter(x, recipe["filter"])
    if recipe.get("echo"):
        x = np.concatenate([x, np.zeros(int(0.5 * SR), dtype=np.float32)])
        x = ffilter(x, "aecho=0.8:0.55:70|160|310:0.45|0.3|0.18")
    if recipe.get("radio"):
        x = radio(x, seed)
    x = trim_silence(x)
    x = fades(x, fade_out=recipe.get("fade_out", 0.02))
    return normalise(x, recipe.get("gain", 0.0))


def credit_line(name, recipe, sources):
    s = sources[recipe["src"]]
    if s["type"] == "tts":
        what = f'espeak-ng voice line "{recipe["text"]}"' + (f' ({recipe["voice"]})' if "voice" in recipe else "")
    elif s["type"] == "github":
        what = f'{s["title"]}: {recipe["file"]} (github.com/{s["repo"]} @ {s["ref"][:10]})'
    else:
        what = f'"{s["title"]}" (commons.wikimedia.org)'
        if "start" in recipe:
            what += f' {recipe["start"]:.2f}s+{recipe.get("dur", 0):.2f}s'
    return f"{name}\t{what}\t{s['author']}\t{s['license']}"


def check(manifest, out_dir):
    ok = True
    credits = open(os.path.join(out_dir, "CREDITS.txt"), encoding="utf-8").read() if os.path.exists(os.path.join(out_dir, "CREDITS.txt")) else ""
    for event, variants in manifest["sounds"].items():
        for i in range(len(variants)):
            name = f"{event}_{i + 1}.ogg"
            path = os.path.join(out_dir, name)
            if not os.path.exists(path):
                print(f"missing: {name}")
                ok = False
                continue
            if name not in credits:
                print(f"not credited: {name}")
                ok = False
            x = decode(path)
            peak = 20 * np.log10(np.abs(x).max() + 1e-12)
            if peak > PEAK_DBFS or len(x) < int(0.005 * SR) or len(x) > 6 * SR:
                print(f"bad level/length: {name} peak {peak:.1f} dBFS, {len(x) / SR:.2f}s")
                ok = False
    print("check:", "OK" if ok else "FAILED")
    return ok


def build(manifest, sources, out_dir, cache, only, title):
    """Renders `manifest["sounds"]` into out_dir with its CREDITS.txt; True when the check passes"""
    sounds = {e: v for e, v in manifest["sounds"].items() if not only or e in only}
    needed = {"github": {}, "commons": set()}
    for variants in sounds.values():
        for r in variants:
            s = sources[r["src"]]
            if s["type"] == "github":
                needed["github"].setdefault(r["src"], set()).add(r["file"])
            elif s["type"] == "commons":
                needed["commons"].add(r["src"])
    local = fetch_sources({"sources": sources}, cache, needed)
    os.makedirs(out_dir, exist_ok=True)
    if not only:
        for f in os.listdir(out_dir):
            if f.endswith(".ogg"):
                os.remove(os.path.join(out_dir, f))
    credits_path = os.path.join(out_dir, "CREDITS.txt")
    lines = {}
    if only and os.path.exists(credits_path):
        for line in open(credits_path, encoding="utf-8"):
            if line.count("\t") >= 3:
                lines[line.split("\t", 1)[0]] = line.rstrip("\n")
    seed = 1
    for event, variants in sounds.items():
        for i, recipe in enumerate(variants):
            name = f"{event}_{i + 1}.ogg"
            x = render(recipe, local, sources, cache, seed)
            seed += 1
            encode(x, os.path.join(out_dir, name))
            lines[name] = credit_line(name, recipe, sources)
            print(f"{name:34} {len(x) / SR:5.2f}s")
    with open(credits_path, "w", encoding="utf-8") as f:
        f.write(title + "\n")
        f.write("Built by tools/prepare_sfx.py. All sources are CC0 or public domain.\n")
        f.write("Kenney packs: www.kenney.nl (CC0). Generated lines: espeak-ng.\n\n")
        f.write("file\tsource\tauthor\tlicence\n")
        for name in sorted(lines):
            f.write(lines[name] + "\n")
    return check(manifest, out_dir)


def build_voices(args):
    """Voice lines of every language of tools/voice_sources.json into <voices>/<language>/"""
    for tool in ("ffmpeg", "curl", "espeak-ng"):
        if not shutil.which(tool):
            return f"{tool} is needed"
    sources = json.load(open(os.path.join(ROOT, "tools", "sfx_sources.json"), encoding="utf-8"))["sources"]
    voices = json.load(open(os.path.join(ROOT, "tools", "voice_sources.json"), encoding="utf-8"))
    out_root = args.out if args.out != os.path.join(ROOT, "assets", "audio", "sfx") else os.path.join(ROOT, "assets", "audio", "voices")
    only = set(filter(None, args.only.split(",")))
    ok = True
    for lang, sounds in voices["languages"].items():
        out_dir = os.path.join(out_root, lang)
        manifest = {"sounds": sounds}
        if args.check:
            ok = check(manifest, out_dir) and ok
        else:
            ok = build(manifest, sources, out_dir, args.cache, only, f"Voice lines ({lang}) of Open Art Of War: Freedom for Liberty") and ok
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", default=os.path.join(ROOT, "tools", "sfx_sources.json"))
    ap.add_argument("--cache", default=os.path.join(os.path.expanduser("~"), ".cache", "openaow-sfx"))
    ap.add_argument("--out", default=os.path.join(ROOT, "assets", "audio", "sfx"))
    ap.add_argument("--only", default="")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--voices", action="store_true", help="build the voice lines of every language (tools/voice_sources.json)")
    args = ap.parse_args()
    if args.voices:
        sys.exit(build_voices(args))
    for tool in ("ffmpeg", "curl", "espeak-ng"):
        if not shutil.which(tool):
            sys.exit(f"{tool} is needed")
    manifest = json.load(open(args.manifest, encoding="utf-8"))
    if args.check:
        sys.exit(0 if check(manifest, args.out) else 1)

    sources = manifest["sources"]
    only = set(filter(None, args.only.split(",")))
    sounds = {e: v for e, v in manifest["sounds"].items() if not only or e in only}
    needed = {"github": {}, "commons": set()}
    for variants in sounds.values():
        for r in variants:
            s = sources[r["src"]]
            if s["type"] == "github":
                needed["github"].setdefault(r["src"], set()).add(r["file"])
            elif s["type"] == "commons":
                needed["commons"].add(r["src"])
    local = fetch_sources(manifest, args.cache, needed)

    os.makedirs(args.out, exist_ok=True)
    if not only:
        for f in os.listdir(args.out):
            if f.endswith(".ogg"):
                os.remove(os.path.join(args.out, f))
    credits_path = os.path.join(args.out, "CREDITS.txt")
    old = {}
    if only and os.path.exists(credits_path):
        for line in open(credits_path, encoding="utf-8"):
            if line.count("\t") >= 3:
                old[line.split("\t", 1)[0]] = line.rstrip("\n")
    lines = dict(old)
    seed = 1
    for event, variants in sounds.items():
        for i, recipe in enumerate(variants):
            name = f"{event}_{i + 1}.ogg"
            x = render(recipe, local, sources, args.cache, seed)
            seed += 1
            encode(x, os.path.join(args.out, name))
            lines[name] = credit_line(name, recipe, sources)
            print(f"{name:34} {len(x) / SR:5.2f}s")
    with open(credits_path, "w", encoding="utf-8") as f:
        f.write("Sound effects of Open Art Of War: Freedom for Liberty\n")
        f.write("Built by tools/prepare_sfx.py from tools/sfx_sources.json. All sources are CC0 or public domain.\n")
        f.write("Kenney packs: www.kenney.nl (CC0). Wikimedia Commons files: commons.wikimedia.org.\n\n")
        f.write("file\tsource\tauthor\tlicence\n")
        for name in sorted(lines):
            f.write(lines[name] + "\n")
    sys.exit(0 if check(manifest, args.out) else 1)


if __name__ == "__main__":
    main()
