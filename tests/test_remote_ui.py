"""The remote core's screens (src/remote/ui/), on the host, with sample services and RetroArch's
bitmap font from vendor/retroarch."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
RETROARCH = ROOT / 'vendor/retroarch'
SOURCES = ['src/remote/ui/remote_ui.cpp', 'src/remote/ui/canvas.cpp', 'src/remote/remote.cpp',
           'src/remote/files.cpp', 'src/remote/backends.cpp', 'src/remote/romm/romm_source.cpp',
           'src/scraper_http.cpp']


class RemoteUi(unittest.TestCase):
    def test_screens(self):
        if not (RETROARCH / 'gfx/drivers_font_renderer/bitmap.h').is_file():
            self.skipTest('vendor/retroarch not fetched (tools/fetch-retroarch.sh fetches it)')
        with tempfile.TemporaryDirectory() as td:
            library = str(Path(td) / 'ps5_library.o')
            subprocess.run(['cc', '-std=c11', '-O2', '-c', 'src/ps5_library.c', '-o', library], cwd=ROOT,
                           check=True)
            binary = str(Path(td) / 'remote-ui-test')
            subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-exceptions',
                            '-fno-rtti', '-pthread', '-Isrc', '-I' + str(RETROARCH), 'tests/remote_ui_test.cpp',
                            *SOURCES, library, '-o', binary], cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True, timeout=60)


if __name__ == '__main__':
    unittest.main()
