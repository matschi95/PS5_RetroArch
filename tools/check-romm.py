#!/usr/bin/env python3
# PS5 RetroArch - Check of the download sources and the save sync against real RomM servers in
# Docker.
# Copyright (C) 2026 Mario Reisinger
# SPDX-License-Identifier: GPL-3.0-or-later
"""check-romm.py [RomM version ...]

For each RomM version (default: the oldest the backends take, romm::minimum_version, the newest
they were checked with, and one older than the oldest, which each of them is to refuse), starts a
RomM in Docker (tools/romm-test/docker-compose.yml) with a few fake SNES games and a BIOS, makes
an admin and a player with client API tokens, scans the library, and runs
tests/romm_live_test.cpp against it as the player; then takes it all down again. The first run of a version downloads its image
(about 1 GB).

Needs Docker with Compose; without it, it says so and is skipped. ROMM_CHECK=0 skips it;
ROMM_KEEP=1 leaves the servers running (their port is printed).
"""

import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from base64 import b64encode
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMPOSE = ROOT / "tools/romm-test/docker-compose.yml"
SCAN = ROOT / "tools/romm-test/scan.py"
# The newest RomM the backends were checked with, and one older than minimum_version.
NEWEST = "5.3.1"
TOO_OLD = "5.2.0"
SCOPES = ["platforms.read", "roms.read", "assets.read", "assets.write", "devices.read", "devices.write", "me.read",
          "firmware.read"]
SOURCES = ["tests/romm_live_test.cpp", "src/remote/remote.cpp", "src/remote/firmware.cpp", "src/remote/library.cpp", "src/remote/save_config.cpp", "src/remote/files.cpp",
           "src/remote/backends.cpp", "src/remote/pairing.cpp", "src/remote/stream_check.cpp", "src/remote/save_sync.cpp",
           "src/remote/romm/romm_client.cpp", "src/remote/romm/romm_source.cpp", "src/remote/romm/romm_firmware.cpp", "src/remote/romm/romm_saves.cpp",
           "src/scraper_http.cpp"]

# The fake games: two, and a large one (downloaded, stopped part way and gone on with).
GAMES = {
    "Alpha Quest (USA).sfc": 512 << 10,
    "Beta Racer (Europe).sfc": 1 << 20,
    "Delta Big (USA).sfc": 48 << 20,
}


def minimum_version():
    text = (ROOT / "src/remote/romm/romm_client.h").read_text()
    return re.search(r'minimum_version\[\] = "([0-9.]+)"', text).group(1)


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def ask(url, path, body=None, user=None):
    request = urllib.request.Request(url + path, method="POST" if body is not None else "GET")
    if body is not None:
        request.data = json.dumps(body).encode()
        request.add_header("Content-Type", "application/json")
    if user:
        request.add_header("Authorization", "Basic " + b64encode(f"{user[0]}:{user[1]}".encode()).decode())
    with urllib.request.urlopen(request, timeout=30) as answer:
        return json.loads(answer.read() or b"null")


def build(work):
    """The check program, with the title's HTTP client for the host (plain http)."""
    objects = []
    for name, source, flags in (("ps5_library.o", "src/ps5_library.c", ["-std=c11"]),
                                ("md5.o", "vendor/retroarch/libretro-common/utils/md5.c",
                                 ["-std=c11", "-Ivendor/retroarch/libretro-common/include"])):
        objects.append(str(work / name))
        subprocess.run(["cc", "-O1", *flags, "-c", source, "-o", objects[-1]], cwd=ROOT, check=True)
    binary = work / "romm-live-test"
    subprocess.run(["c++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fno-exceptions", "-fno-rtti",
                    "-pthread", "-Isrc", "-Ivendor/retroarch/libretro-common/include", *SOURCES, *objects, "-lz",
                    "-o", str(binary)], cwd=ROOT, check=True)
    return binary


def run_version(version, binary, work):
    """Starts a RomM of `version`, checks against it, and takes it down; True when it passed."""
    project = "ps5-retroarch-romm-check-" + re.sub(r"[^a-z0-9]", "", version.lower())
    library = work / f"library-{version}"
    for name, size in GAMES.items():
        path = library / "roms/snes" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("wb") as out:
            for at in range(0, size, 8 << 20):
                out.write(os.urandom(min(8 << 20, size - at)))
    # A BIOS of the platform, which RomM's scan takes as its firmware.
    bios = library / "bios/snes/BS-X.bin"
    bios.parent.mkdir(parents=True, exist_ok=True)
    bios.write_bytes(os.urandom(1 << 20))
    port = free_port()
    env = dict(os.environ, ROMM_VERSION=version, ROMM_PORT=str(port), ROMM_LIBRARY=str(library))
    compose = ["docker", "compose", "-p", project, "-f", str(COMPOSE)]
    url = f"http://127.0.0.1:{port}"
    try:
        print(f"check-romm: starting RomM {version} on port {port}", flush=True)
        started = subprocess.run([*compose, "up", "-d", "--quiet-pull"], env=env, capture_output=True, text=True)
        if started.returncode != 0:
            print(f"check-romm: RomM {version} did not start:\n{started.stderr[-2000:]}")
            return False
        for _ in range(180):
            try:
                if ask(url, "/api/heartbeat")["SYSTEM"]["VERSION"]:
                    break
            except (urllib.error.URLError, ConnectionError, KeyError, ValueError):
                time.sleep(1)
        else:
            print(f"check-romm: RomM {version} did not start")
            return False
        admin, player = ("admin", "admin-check"), ("player", "player-check")
        ask(url, "/api/users", {"username": admin[0], "email": "admin@example.org", "password": admin[1], "role": "admin"})
        ask(url, "/api/users", {"username": player[0], "email": "player@example.org", "password": player[1],
                                "role": "editor"}, user=admin)
        subprocess.run([*compose, "cp", str(SCAN), "romm:/tmp/scan.py"], env=env, check=True, capture_output=True)
        scanned = subprocess.run([*compose, "exec", "-T", "romm", "python", "/tmp/scan.py", *admin], env=env,
                                 capture_output=True, text=True, timeout=300)
        print("check-romm:", scanned.stdout.strip().splitlines()[-1] if scanned.stdout.strip() else scanned.stderr[-500:])
        old = version == TOO_OLD
        if old:
            # The player signs in with the password: the server is refused before scopes matter.
            token = bare = None
        else:
            token = ask(url, "/api/client-tokens", {"name": "check", "scopes": SCOPES}, user=player)["raw_token"]
            bare = ask(url, "/api/client-tokens", {"name": "bare", "scopes": ["roms.read"]}, user=player)["raw_token"]
        folder = work / f"consoles-{version}"
        folder.mkdir()
        return subprocess.run([str(binary), url, token or "", bare or "", str(folder), str(library), "1" if old else "0"],
                              env=dict(os.environ, ROMM_LIVE_USER=":".join(player)), timeout=1200).returncode == 0
    finally:
        if os.environ.get("ROMM_KEEP"):
            print(f"check-romm: RomM {version} left running at {url} (docker compose -p {project} down -v)")
        else:
            subprocess.run([*compose, "down", "-v"], env=env, capture_output=True)


def main():
    if os.environ.get("ROMM_CHECK") == "0":
        print("check-romm: SKIPPED (ROMM_CHECK=0)")
        return 0
    if shutil.which("docker") is None or subprocess.run(["docker", "compose", "version"],
                                                        capture_output=True).returncode != 0:
        print("check-romm: SKIPPED (no Docker with Compose)")
        return 0
    versions = sys.argv[1:] or [minimum_version(), NEWEST, TOO_OLD]
    with tempfile.TemporaryDirectory(prefix="romm-check-") as work:
        work = Path(work)
        binary = build(work)
        results = {version: run_version(version, binary, work) for version in dict.fromkeys(versions)}
    for version, passed in results.items():
        print(f"check-romm: RomM {version}: {'PASS' if passed else 'FAIL'}")
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
