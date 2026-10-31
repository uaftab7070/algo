#!/bin/sh
# Builds ahpv and the unit tests. POSIX sh.
#   scripts/build.sh                 host build in build/host
#   scripts/build.sh sanitize        ASan/UBSan build in build/asan
#   CXX=<sdk>-g++ BUILD=build/rv1106 TARGET_OS=Linux scripts/build.sh
#     cross build; the SDK toolchain name must be taken from the installed
#     Luckfox SDK (docs/sdk-compatibility.md). Cross builds are not run here.
set -eu
cd "$(dirname "$0")/.."
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
if [ "${1:-}" = sanitize ]; then
    exec make -j"$JOBS" sanitize
fi
make -j"$JOBS" ${BUILD:+BUILD="$BUILD"} ${CXX:+CXX="$CXX"} ${TARGET_OS:+TARGET_OS="$TARGET_OS"}
BIN="${BUILD:-build/host}/ahpv"
echo "built $BIN"
if [ -z "${CXX:-}" ]; then "$BIN" version; fi
