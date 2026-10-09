#!/usr/bin/env bash
# Build the remote core of the download sources (cores/remote/, src/remote/remote.h) with
# this title's SDK. Its screens are eboot.bin's (src/remote/remote_core.h): it imports
# them, and needs only libretro.h, from the pinned tree in vendor/retroarch.
# Output: build/cores/stage/{cores,info}/; build-title.sh stages these in /app0.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
source "$root/tools/core-stamp.sh"
core_stamp_skip remote \
    "$root/build/cores/stage/cores/remote_libretro.so" \
    "$root/build/cores/stage/info/remote_libretro.info" \
    -- "$root/tools/build-remote.sh" "$root/cores/remote" "$root/src/remote/remote_core.h"
[[ $# == 0 ]] || { echo "usage: ${0##*/}" >&2; exit 2; }
sdk="$root/.deps/native/ps5-payload-sdk"
[[ -x $sdk/bin/prospero-clang ]] || { echo "error: bootstrap this project's SDK first" >&2; exit 2; }
retroarch="$root/vendor/retroarch"
[[ -f $retroarch/libretro-common/include/libretro.h ]] || {
    echo "error: vendor/retroarch is missing; run tools/fetch-retroarch.sh" >&2; exit 2; }
export PS5_PAYLOAD_SDK="$sdk"
export PS5_CLANG=/usr/bin/clang
work="$root/build/cores/remote"
stage="$root/build/cores/stage"
rm -rf -- "$work"
mkdir -p "$work" "$stage/cores" "$stage/info"
# As the other cores are linked: no SDK libc or CRT, the title binds the imports (its own
# functions included, hence -z undefs).
"$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -shared \
    -I"$retroarch/libretro-common/include" -I"$root/src" \
    "$root/cores/remote/remote_libretro.c" -o "$work/remote_libretro.so" \
    -nostdlib -nodefaultlibs -Wl,-z,undefs -Wl,--build-id=sha1 -Wl,-T,"$root/tooling/native/ps5-core.ld" \
    -lkernel_web -lSceLibcInternal -lScePosixForWebKit
python3 tools/check-core.py "$work/remote_libretro.so" --report "$work/abi.json"
cp -- "$work/remote_libretro.so" "$stage/cores/remote_libretro.so"
cp -- "$root/cores/remote/remote_libretro.info" "$stage/info/remote_libretro.info"
python3 - "$work" "$(git -C "$root" rev-parse HEAD)" <<'PY'
import json, pathlib, sys
work = pathlib.Path(sys.argv[1])
report = json.loads((work / 'abi.json').read_text())
report.update(source_revision=sys.argv[2])
(work / 'build.json').write_text(json.dumps(report, indent=2) + '\n')
PY
core_stamp_write
printf '==> [remote] built and ABI-checked the remote core; console loading is a separate gate\n'
