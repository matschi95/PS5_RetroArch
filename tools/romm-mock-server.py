#!/usr/bin/env python3
# PS5 RetroArch - a stand-in for a RomM server, for the cases a real one does not show on demand.
# Copyright (C) 2026 Mario Reisinger
# SPDX-License-Identifier: GPL-3.0-or-later
"""romm-mock-server.py <port file>

For tests/test_romm.py: what tests/romm_test.cpp checks of the RomM backend. Serves
/api/heartbeat (RomM 5.3.1; under /old, 5.2.0), /api/platforms, /api/roms (a page), a cover and
/api/roms/<file id>/files/content/<name> (with Range) on 127.0.0.1 and a free port it writes to <port file>, signed in as "Bearer rmm_test" or
"Basic" for player:secret. Files are made of a pattern of their id, so the check can tell every
byte.

- A page has three games at most, whatever was asked for (as a server or a proxy may cap it).
- ROM 10 has a ScreenScraper id and ROM 16 is a second copy of it; ROM 11 is a game in a folder
  of its own (a .cue and its track), with a manual that stays on the server.
- ROM 11's track ignores Range (a server that cannot resume).
- ROM 13 has no name: its file's name stands for it; ROM 14 is of a platform the title does not
  know; ROM 15 has only a patch (no file of the game).
"""

import http.server
import json
import re
import sys
import urllib.parse

TOKENS = ("Bearer rmm_test", "Basic cGxheWVyOnNlY3JldA==")  # player:secret


def pattern(file_id, size):
    return bytes(((i * 7 + file_id) & 0xFF) for i in range(size))


# file id -> (path under the platform's folder, size, category)
FILES = {
    100: ("Alpha Quest (USA).sfc", 6_000_000, None),
    110: ("Beta Racer/Beta Racer.cue", 120, None),
    111: ("Beta Racer/Beta Racer (Track 1).bin", 5_000_000, None),
    112: ("Beta Racer/manual/Beta Racer.pdf", 1000, "manual"),
    130: ("Delta (Europe).sfc", 1000, None),
    140: ("Epsilon.xyz", 1000, None),
    150: ("Zeta (Translated).ips", 400, "translation"),
    160: ("Alpha Quest (Rev 1).sfc", 1000, None),
}
PLATFORMS = {"snes": 2, "psx": 4, "foo": 9}
ROMS = [
    {"id": 10, "name": "Alpha Quest", "platform": "snes", "fs_name": "Alpha Quest (USA).sfc",
     "files": [100], "is_identified": True, "ss_id": 1000, "igdb_id": None,
     "path_cover_small": "roms/2/10/cover/small.png?ts=1"},
    {"id": 11, "name": "Beta Racer", "platform": "psx", "fs_name": "Beta Racer", "files": [110, 111, 112],
     "is_identified": True},
    {"id": 13, "name": None, "platform": "snes", "fs_name": "Delta (Europe).sfc", "files": [130]},
    {"id": 14, "name": "Epsilon", "platform": "foo", "fs_name": "Epsilon.xyz", "files": [140]},
    {"id": 15, "name": "Zeta", "platform": "snes", "fs_name": "Zeta", "files": [150]},
    {"id": 16, "name": "Alpha Quest", "platform": "snes", "fs_name": "Alpha Quest (Rev 1).sfc", "files": [160],
     "is_identified": True, "ss_id": 1000},
]
MOST_PER_PAGE = 3
NO_RANGE = {111}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def send(self, status, body, kind="application/json", headers=()):
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(url.query)
        if url.path in ("/api/heartbeat", "/old/api/heartbeat"):
            version = "5.2.0" if url.path.startswith("/old") else "5.3.1"
            return self.send(200, json.dumps({"SYSTEM": {"VERSION": version}}).encode())
        if self.headers.get("Authorization") not in TOKENS:
            return self.send(401, b'{"detail":"Unauthorized"}')
        if url.path == "/api/platforms":
            return self.send(200, json.dumps([{"id": i, "slug": s, "fs_slug": s} for s, i in PLATFORMS.items()])
                             .encode())
        if url.path == "/api/roms":
            wanted = set(query.get("platform_ids", []))
            limit = min(int(query.get("limit", ["50"])[0]), MOST_PER_PAGE)
            offset = int(query.get("offset", ["0"])[0])
            items = []
            for rom in ROMS:
                if wanted and str(PLATFORMS[rom["platform"]]) not in wanted:
                    continue
                item = {k: v for k, v in rom.items() if k not in ("platform", "files")}
                item.update(platform_slug=rom["platform"], platform_fs_slug=rom["platform"],
                            fs_path=rom["platform"], missing_from_fs=False,
                            fs_name_no_ext=rom["fs_name"].rsplit(".", 1)[0] if "." in rom["fs_name"]
                            else rom["fs_name"])
                item["files"] = [{"id": f, "file_name": FILES[f][0].split("/")[-1],
                                  "full_path": rom["platform"] + "/" + FILES[f][0],
                                  "file_size_bytes": FILES[f][1], "category": FILES[f][2],
                                  "crc_hash": None, "md5_hash": None, "sha1_hash": None}
                                 for f in rom["files"]]
                items.append(item)
            page = items[offset:offset + limit]
            return self.send(200, json.dumps({"items": page, "total": len(items), "limit": limit,
                                              "offset": offset}).encode())
        if url.path == "/assets/romm/resources/roms/2/10/cover/small.png":
            return self.send(200, b"\x89PNG alpha", "image/png")
        match = re.fullmatch(r"/api/roms/(\d+)/files/content/(.+)", url.path)
        if match:
            file_id = int(match.group(1))
            if file_id not in FILES or urllib.parse.unquote(match.group(2)) != FILES[file_id][0].split("/")[-1]:
                return self.send(404, b'{"detail":"File not found"}')
            size = FILES[file_id][1]
            start = 0
            ranged = self.headers.get("Range")
            if ranged and file_id not in NO_RANGE:
                start = int(re.fullmatch(r"bytes=(\d+)-", ranged).group(1))
                if start >= size:
                    return self.send(416, b"", headers=[("Content-Range", f"bytes */{size}")])
            body = pattern(file_id, size)[start:]
            headers = [("Content-Range", f"bytes {start}-{size - 1}/{size}")] if start else []
            try:
                return self.send(206 if start else 200, body, "application/octet-stream", headers)
            except (BrokenPipeError, ConnectionResetError):
                return None
        return self.send(404, b'{"detail":"Not Found"}')


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    with open(sys.argv[1], "w") as out:
        out.write(str(server.server_address[1]))
    server.serve_forever()


if __name__ == "__main__":
    main()
