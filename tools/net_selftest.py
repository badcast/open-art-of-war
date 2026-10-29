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
    exe = args[0] if args else "build/openaow"
    seconds = int(args[1]) if len(args) > 1 else 40
    game_map = args[2] if len(args) > 2 else "brod"
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", OAOW_NET_TRACE="1", OAOW_NET_BOT="1")
    procs = []
    if hosted:
        server_exe = os.path.join(os.path.dirname(exe) or ".", "openaow-server" + (".exe" if exe.endswith(".exe") else ""))
        procs.append(start([server_exe, "--port", "38160", "--room-ports", "38161-38165", "--tcp-port", "0",
                            "--stats-interval-s", "0", "--name", "selftest"], env))
        time.sleep(0.5)
        first = start([exe, "--host=38196", "--players=2", f"--map={game_map}", "--name=Host",
                       "--server=127.0.0.1:38160"], env)
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
        procs.append(start([exe, "--server=127.0.0.1:38160", f"--server-code={code}", "--name=Guest"], env))
        players = procs[1:]
    elif via_server:
        server_exe = os.path.join(os.path.dirname(exe) or ".", "openaow-server" + (".exe" if exe.endswith(".exe") else ""))
        procs.append(start([server_exe, "--port", "38160", "--room-ports", "38161-38165", "--tcp-port", "0",
                            "--stats-interval-s", "0", "--name", "selftest"], env))
        time.sleep(0.5)
        first = start([exe, "--server=127.0.0.1:38160", "--server-create", "--players=2", f"--map={game_map}", "--name=Owner"], env)
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
        procs.append(start([exe, "--server=127.0.0.1:38160", f"--server-code={code}", "--name=Guest"], env))
        players = procs[1:]
    else:
        port = "38199"
        procs.append(start([exe, f"--host={port}", "--players=2", f"--map={game_map}", "--name=Host"], env))
        time.sleep(1.0)
        guest_env = dict(env, OAOW_NET_DROP_AT="300") if reconnect else env
        procs.append(start([exe, f"--join=127.0.0.1:{port}", "--name=Guest"], guest_env))
        players = procs
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
    if reconnect and "Reconnected" not in outputs[1]:
        print("FAILED: the second player did not come back")
        print(outputs[1][-1500:])
        return 1
    if desync or mismatched:
        print("FAILED: the peers drifted apart")
        return 1
    print(f"OK: {len(common)} checksums equal, no desync")
    return 0


if __name__ == "__main__":
    sys.exit(main())
