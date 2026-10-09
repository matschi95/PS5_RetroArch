"""The save sync (src/remote/save_sync.h), on the host, with a store of the test's own."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SOURCES = ['src/remote/save_sync.cpp', 'src/remote/save_jobs.cpp', 'src/remote/save_config.cpp', 'src/remote/remote.cpp', 'src/remote/firmware.cpp',
           'src/remote/library.cpp', 'src/remote/files.cpp', 'src/remote/stream_check.cpp',
           'src/remote/romm/romm_saves.cpp', 'src/remote/romm/romm_client.cpp', 'src/scraper_http.cpp']


class SaveSync(unittest.TestCase):
    def test_sync(self):
        with tempfile.TemporaryDirectory() as td:
            library = str(Path(td) / 'ps5_library.o')
            subprocess.run(['cc', '-std=c11', '-O2', '-c', 'src/ps5_library.c', '-o', library], cwd=ROOT,
                           check=True)
            md5 = str(Path(td) / 'md5.o')
            subprocess.run(['cc', '-std=c11', '-O2', '-Ivendor/retroarch/libretro-common/include', '-c',
                            'vendor/retroarch/libretro-common/utils/md5.c', '-o', md5], cwd=ROOT, check=True)
            binary = str(Path(td) / 'save-sync-test')
            subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-exceptions',
                            '-fno-rtti', '-pthread', '-Isrc', '-Ivendor/retroarch/libretro-common/include',
                            'tests/save_sync_test.cpp', *SOURCES, library, md5, '-lz', '-o', binary],
                           cwd=ROOT, check=True)
            subprocess.run([binary, td], cwd=ROOT, check=True, timeout=60)


if __name__ == '__main__':
    unittest.main()
