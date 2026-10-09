"""Builds the WebUI's server for the host (the tests' fixtures): the given main, the
server's sources and its libraries, with defines. One list of sources for every test."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
SOURCES = ['src/webui_ps5.cpp', 'src/webui_transfer.cpp', 'src/webui_update.cpp', 'src/scraper.cpp',
           'src/scraper_http.cpp',
           # The game servers' setup (src/remote/servers.h) and the backends it signs in with.
           'src/remote/servers.cpp', 'src/remote/backends.cpp', 'src/remote/files.cpp',
           'src/remote/save_config.cpp', 'src/remote/romm/romm_client.cpp',
           'src/remote/romm/romm_source.cpp', 'src/remote/romm/romm_firmware.cpp',
           'src/remote/romm/romm_saves.cpp', 'src/remote/romm/romm_setup.cpp']
C_SOURCES = ['src/ps5_library.c']


def build(output, main, defines=(), flags=('-O1', '-g')):
    output = Path(output)
    http = subprocess.check_output(['bash', 'tools/build-webui-http.sh', 'host'], cwd=ROOT, text=True).strip()
    update = subprocess.check_output(['bash', 'tools/build-webui-update.sh', 'host'], cwd=ROOT, text=True).strip()
    objects = []
    for source in C_SOURCES:
        obj = output.with_name(output.name + '-' + Path(source).stem + '.o')
        subprocess.run(['cc', '-std=c11', '-D_DEFAULT_SOURCE', *flags, '-c', source, '-o', str(obj)], cwd=ROOT, check=True)
        objects.append(str(obj))
    subprocess.run(['c++', '-std=c++17', *flags, '-pthread', '-Isrc',
                    '-I' + str(ROOT / '.deps/webui/libmicrohttpd-1.0.10/src/include'),
                    '-I' + str(ROOT / '.deps/native/zlib/zlib-1.3.2/contrib/minizip'),
                    '-I' + str(ROOT / 'vendor/retroarch/deps/mbedtls'),
                    '-DPS5_SCRAPER_CURL',  # the daemon's HTTP client, the host's libcurl
                    *[f'-D{d}' for d in defines], main, *SOURCES, *objects, http, update, '-lz', '-lcurl',
                    '-o', str(output)], cwd=ROOT, check=True)
    return output
