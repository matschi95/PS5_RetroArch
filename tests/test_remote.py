"""The download sources (src/remote/), on the host, with a backend of the test's own."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SOURCES = ['src/remote/remote.cpp', 'src/remote/firmware.cpp', 'src/remote/library.cpp', 'src/remote/save_config.cpp', 'src/remote/files.cpp', 'src/remote/stream_check.cpp']


class Remote(unittest.TestCase):
    def test_sources(self):
        with tempfile.TemporaryDirectory() as td:
            library = str(Path(td) / 'ps5_library.o')
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-c', 'src/ps5_library.c',
                            '-o', library], cwd=ROOT, check=True)
            binary = str(Path(td) / 'remote-test')
            subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-exceptions',
                            '-fno-rtti', '-pthread', '-Isrc', 'tests/remote_test.cpp', *SOURCES, library,
                            '-lz', '-o', binary], cwd=ROOT, check=True)
            work = Path(td) / 'title'
            work.mkdir()
            subprocess.run([binary, str(work)], cwd=ROOT, check=True, timeout=120)


if __name__ == '__main__':
    unittest.main()
