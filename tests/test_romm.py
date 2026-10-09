"""The RomM backend of the download sources (src/remote/romm/), on the host, against
tools/romm-mock-server.py."""
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
SOURCES = ['src/remote/remote.cpp', 'src/remote/firmware.cpp', 'src/remote/library.cpp', 'src/remote/save_config.cpp', 'src/remote/files.cpp', 'src/remote/stream_check.cpp', 'src/remote/backends.cpp',
           'src/remote/romm/romm_source.cpp', 'src/remote/romm/romm_firmware.cpp', 'src/remote/romm/romm_setup.cpp', 'src/remote/servers.cpp', 'src/remote/romm/romm_client.cpp', 'src/remote/romm/romm_saves.cpp', 'src/scraper_http.cpp']


class Romm(unittest.TestCase):
    def test_backend(self):
        with tempfile.TemporaryDirectory() as td:
            library = str(Path(td) / 'ps5_library.o')
            subprocess.run(['cc', '-std=c11', '-O2', '-c', 'src/ps5_library.c', '-o', library], cwd=ROOT,
                           check=True)
            binary = str(Path(td) / 'romm-test')
            subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-exceptions',
                            '-fno-rtti', '-pthread', '-Isrc', 'tests/romm_test.cpp', *SOURCES, library, '-lz',
                            '-o', binary], cwd=ROOT, check=True)
            port_file = Path(td) / 'port'
            server = subprocess.Popen([sys.executable, str(ROOT / 'tools/romm-mock-server.py'), str(port_file)])
            try:
                for _ in range(100):
                    if port_file.exists() and port_file.read_text():
                        break
                    time.sleep(0.05)
                work = Path(td) / 'title'
                work.mkdir()
                subprocess.run([binary, f'http://127.0.0.1:{port_file.read_text()}', str(work)], cwd=ROOT,
                               check=True, timeout=120)
            finally:
                server.terminate()
                server.wait()


if __name__ == '__main__':
    unittest.main()
