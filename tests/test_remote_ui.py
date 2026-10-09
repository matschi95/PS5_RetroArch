"""The remote core's screens (src/remote/ui/), on the host, with sample services and RetroArch's
bitmap font from vendor/retroarch."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
RETROARCH = ROOT / 'vendor/retroarch'
SOURCES = ['src/remote/ui/remote_ui.cpp', 'src/remote/ui/canvas.cpp', 'src/remote/remote.cpp', 'src/remote/firmware.cpp',
           'src/remote/files.cpp', 'src/remote/stream_check.cpp', 'src/remote/backends.cpp', 'src/remote/romm/romm_source.cpp', 'src/remote/romm/romm_firmware.cpp', 'src/remote/romm/romm_setup.cpp', 'src/remote/servers.cpp', 'src/remote/romm/romm_client.cpp', 'src/remote/romm/romm_saves.cpp',
           'src/remote/pairing.cpp', 'src/remote/save_config.cpp', 'src/scraper_http.cpp']
INCLUDES = ['-Isrc', '-Ithird_party', '-I' + str(RETROARCH)]


def c_objects(folder):
    """The C parts, built as C: the library reader and the QR code generator."""
    objects = []
    for source in ('src/ps5_library.c', 'third_party/qrcodegen/qrcodegen.c'):
        objects.append(str(Path(folder) / (Path(source).stem + '.o')))
        subprocess.run(['cc', '-std=c11', '-O2', '-c', source, '-o', objects[-1]], cwd=ROOT, check=True)
    return objects


class RemoteUi(unittest.TestCase):
    def test_screens(self):
        if not (RETROARCH / 'gfx/drivers_font_renderer/bitmap.h').is_file():
            self.skipTest('vendor/retroarch not fetched (tools/fetch-retroarch.sh fetches it)')
        with tempfile.TemporaryDirectory() as td:
            objects = c_objects(td)
            binary = str(Path(td) / 'remote-ui-test')
            subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-fno-exceptions',
                            '-fno-rtti', '-pthread', *INCLUDES, 'tests/remote_ui_test.cpp',
                            *SOURCES, *objects, '-lz', '-o', binary], cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True, timeout=60)


if __name__ == '__main__':
    unittest.main()
