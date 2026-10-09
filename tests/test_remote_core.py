"""The remote core (cores/remote/), on the host, with libretro.h from vendor/retroarch."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
RETROARCH = ROOT / 'vendor/retroarch'


class RemoteCore(unittest.TestCase):
    def test_core(self):
        if not (RETROARCH / 'libretro-common/include/libretro.h').is_file():
            self.skipTest('vendor/retroarch not fetched (tools/fetch-retroarch.sh fetches it)')
        with tempfile.TemporaryDirectory() as td:
            binary = str(Path(td) / 'remote-core-test')
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(RETROARCH / 'libretro-common/include'), '-Isrc',
                            'tests/remote_core_test.c', '-o', binary], cwd=ROOT, check=True)
            subprocess.run([binary], cwd=ROOT, check=True, timeout=15)


if __name__ == '__main__':
    unittest.main()
