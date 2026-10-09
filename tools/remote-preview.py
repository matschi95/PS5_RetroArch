#!/usr/bin/env python3
"""Pictures of the remote core's screens, drawn on the host with sample data.

    python3 tools/remote-preview.py <folder>

Builds tests/remote_ui_test.cpp (as tests/test_remote_ui.py does), lets it write each screen's
frame, and writes them as PNGs into the folder: the screens can be looked at without a console.
"""
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests'))
from test_remote_ui import RETROARCH, SOURCES  # noqa: E402

WIDTH, HEIGHT = 960, 540


def png(raw, path):
    """XRGB8888 (little-endian: B, G, R, X) as an RGB PNG."""
    rgb = bytearray(WIDTH * HEIGHT * 3)
    rgb[0::3], rgb[1::3], rgb[2::3] = raw[2::4], raw[1::4], raw[0::4]
    rows = b''.join(b'\0' + bytes(rgb[y * WIDTH * 3:(y + 1) * WIDTH * 3]) for y in range(HEIGHT))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', WIDTH, HEIGHT, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as td:
        library = str(Path(td) / 'ps5_library.o')
        subprocess.run(['cc', '-std=c11', '-O2', '-c', 'src/ps5_library.c', '-o', library], cwd=ROOT, check=True)
        binary = str(Path(td) / 'remote-ui-test')
        subprocess.run(['c++', '-std=c++20', '-O2', '-fno-exceptions', '-fno-rtti', '-pthread', '-Isrc',
                        '-I' + str(RETROARCH), 'tests/remote_ui_test.cpp', *SOURCES, library, '-lz', '-o', binary],
                       cwd=ROOT, check=True)
        subprocess.run([binary, td], cwd=ROOT, check=True)
        for frame in sorted(Path(td).glob('*.rgb')):
            png(frame.read_bytes(), out / (frame.stem + '.png'))
            print(out / (frame.stem + '.png'))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
