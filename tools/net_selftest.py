#!/usr/bin/env python3
"""Two game processes play a network match against each other on localhost (no window, no sound;
OAOW_NET_BOT makes both send their units at the enemy). Passes when both report the same world
checksums and nobody announces a desync.

    tools/net_selftest.py <path to openaow> [seconds] [map] [--server]

--server: through openaow-server (next to the game executable): the first player creates a room,
the second joins it by its code.
--hosted: the first player hosts and lists the game on openaow-server, the second joins it by its code
(through the server's introduction and hole punching).
--reconnect: the second player's connection drops at tick 300; it has to come back, play the match
through from its start and stay in sync.
--leave: the second player quits the game halfway through; the first one has to see it leave and
destroy its forces (the turn stream announces it).
--remote=host[:port]: with --server / --hosted, use that server (e.g. openaow.imister.tech) instead of
starting a local openaow-server.

Needs a game built with -DAOW_DEBUG=ON: the OAOW_* variables below are ignored otherwise.

Both players get different factions (OAOW_NET_FACTION_BY_SLOT) and the bot also trains units and places
buildings, so the peers must agree on who plays what and on every unit created.
"""
import os
import re
import subprocess
import sys
import threading
import time


def reader(proc, lines):
    for line in proc.stdout:
        lines.append(line)


def start(cmd, env):
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True)
    lines = []
    threading.Thread(target=reader, args=(proc, lines), daemon=True).start()
    return proc, lines


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    via_server = "--server" in sys.argv
    hosted = "--hosted" in sys.argv
    reconnect = "--reconnect" in sys.argv
    leave = "--leave" in sys.argv
    remote = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--remote=")), None)
    server_addr = remote if remote else "127.0.0.1:38160"
    exe = args[0] if args else "build/openaow"
    seconds = int(args[1]) if len(args) > 1 else 40
    game_map = args[2] if len(args) > 2 else "brod"
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", OAOW_NET_TRACE="1", OAOW_NET_BOT="1",
               OAOW_NET_FACTION_BY_SLOT="1")
    procs = []
    if hosted:
        if not remote:
            server_exe = os.path.join(os.path.dirname(exe) or ".", "openaow-server" + (".exe" if exe.endswith(".exe") else ""))
            procs.append(start([server_exe, "--port", "38160", "--room-ports", "38161-38165", "--tcp-port", "0",
                                "--stats-interval-s", "0", "--name", "selftest"], env))
            time.sleep(0.5)
        first = start([exe, "--host=38196", "--players=2", f"--map={game_map}", "--name=Host",
                       f"--server={server_addr}"], env)
        procs.append(first)
        code = None
        for _ in range(100):
            for line in first[1]:
                m = re.search(r"\[Net\] Listed on the server as ([A-Z0-9]{6})", line)
                if m:
                    code = m.group(1)
            if code:
                break
            time.sleep(0.1)
        if not code:
            print("FAILED: the hosted game was not listed")
            print("".join(first[1])[-2000:])
            for p, _ in procs:
                p.terminate()
            return 1
        print(f"hosted game {code}")
        guest_env = dict(env, OAOW_NET_DROP_AT="300") if reconnect else env
        procs.append(start([exe, f"--server={server_addr}", f"--server-code={code}", "--name=Guest"], guest_env))
        players = procs[-2:]
    elif via_server:
        if not remote:
            server_exe = os.path.join(os.path.dirname(exe) or ".", "openaow-server" + (".exe" if exe.endswith(".exe") else ""))
            procs.append(start([server_exe, "--port", "38160", "--room-ports", "38161-38165", "--tcp-port", "0",
                                "--stats-interval-s", "0", "--name", "selftest"], env))
            time.sleep(0.5)
        first = start([exe, f"--server={server_addr}", "--server-create", "--players=2", f"--map={game_map}", "--name=Owner"], env)
        procs.append(first)
        code = None
        for _ in range(100):
            for line in first[1]:
                m = re.search(r"\[Net\] Room ([A-Z0-9]{6})", line)
                if m:
                    code = m.group(1)
            if code:
                break
            time.sleep(0.1)
        if not code:
            print("FAILED: no room was created")
            print("".join(first[1])[-2000:])
            for p, _ in procs:
                p.terminate()
            return 1
        print(f"room {code}")
        guest_env = dict(env, OAOW_NET_DROP_AT="300") if reconnect else env
        procs.append(start([exe, f"--server={server_addr}", f"--server-code={code}", "--name=Guest"], guest_env))
        players = procs[-2:]
    else:
        port = "38199"
        procs.append(start([exe, f"--host={port}", "--players=2", f"--map={game_map}", "--name=Host"], env))
        time.sleep(1.0)
        guest_env = dict(env, OAOW_NET_DROP_AT="300") if reconnect else env
        procs.append(start([exe, f"--join=127.0.0.1:{port}", "--name=Guest"], guest_env))
        players = procs
    if leave:
        time.sleep(seconds / 2)
        players[1][0].terminate()   # SIGTERM: the game quits the normal way (it leaves the match)
        time.sleep(seconds / 2)
    else:
        time.sleep(seconds)
    for p, _ in reversed(procs):
        p.terminate()
    for p, _ in procs:
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
    outputs = ["".join(lines) for _, lines in players]
    pattern = re.compile(r"\[Net\] tick (\d+) checksum ([0-9a-f]+)")
    sums = [dict(pattern.findall(out)) for out in outputs]
    common = sorted(set(sums[0]) & set(sums[1]), key=int)
    desync = any("DESYNC" in out for out in outputs)
    mismatched = [t for t in common if sums[0][t] != sums[1][t]]
    for t in common:
        print(f"tick {t:>6}: first {sums[0][t]}  second {sums[1][t]}" + ("  <-- differs" if t in mismatched else ""))
    if len(common) < 3:
        print("FAILED: too few checksums to compare (did the match start?)")
        print(outputs[0][-1500:])
        print(outputs[1][-1500:])
        return 1
    if leave:
        if "left the match: its forces are destroyed" not in outputs[0] or "(slot 1) left the match" not in outputs[0]:
            print("FAILED: the first player did not see the second one leave (or kept its forces)")
            print(outputs[0][-2000:])
            return 1
        print("the second player left: the first one saw it and destroyed its forces")
    if reconnect and "Reconnected" not in outputs[1]:
        print("FAILED: the second player did not come back")
        print(outputs[1][-1500:])
        return 1
    if desync or mismatched:
        print("FAILED: the peers drifted apart")
        return 1
    # Who plays what: the same on both computers, and both factions in the match
    slot_pattern = re.compile(r"\[Net\] Slot (\d+): faction (\d+) team (\d+) color (\d+)")
    slots = [sorted(set(slot_pattern.findall(out))) for out in outputs]
    for i, s in enumerate(slots):
        print(f"player {i + 1} sees slots: " + ", ".join(f"{a}: faction {b} team {c} color {d}" for a, b, c, d in s))
    if not slots[0] or slots[0] != slots[1]:
        print("FAILED: the peers disagree on the players' factions / teams / colours")
        return 1
    if len({f for _, f, _, _ in slots[0]}) < 2:
        print("FAILED: both players got the same faction (OAOW_NET_FACTION_BY_SLOT ignored?)")
        return 1
    print(f"OK: {len(common)} checksums equal, no desync")
    return 0


if __name__ == "__main__":
    sys.exit(main())
