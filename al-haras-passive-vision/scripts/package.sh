#!/bin/sh
# Builds a release package: releases/ahpv-<version>-<os>-<arch>.tar.gz with
# SHA256SUMS. Board package (verified 2026-10-25 with Zig 0.13.0, static musl):
#   CXX="zig c++ -target arm-linux-musleabihf -mcpu=cortex_a7" AR="zig ar" \
#   LDFLAGS=-static OBJCOPY="zig objcopy" TARGET_OS=Linux TARGET_ARCH=armv7 \
#   BUILD=build/rv1106 scripts/package.sh
set -eu
cd "$(dirname "$0")/.."
VERSION=$(cat VERSION)
BUILD=${BUILD:-build/host}
OS=${TARGET_OS:-$(uname -s)}
ARCH=${TARGET_ARCH:-$(uname -m)}
make -j4 BUILD="$BUILD" ${CXX:+CXX="$CXX"} ${AR:+AR="$AR"} ${LDFLAGS:+LDFLAGS="$LDFLAGS"} TARGET_OS="$OS" >/dev/null
python3 -I tests/fixtures/make_fixtures.py tests/fixtures/generated >/dev/null 2>&1 || echo "package: python3 unavailable; fixtures omitted" >&2
NAME="ahpv-$VERSION-$(echo "$OS" | tr 'A-Z' 'a-z')-$ARCH"
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/ahpv-package-XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
ROOT="$STAGE/$NAME"
mkdir -p "$ROOT/bin" "$ROOT/config" "$ROOT/schemas" "$ROOT/docs" "$ROOT/scripts" "$ROOT/tools" "$ROOT/deploy" "$ROOT/fixtures" "$ROOT/tests"
for b in ahpv ahpv-unit-tests; do
    if [ -n "${OBJCOPY:-}" ]; then $OBJCOPY --strip-all "$BUILD/$b" "$ROOT/bin/$b"; else cp "$BUILD/$b" "$ROOT/bin/$b"; fi
done
cp config/*.yaml "$ROOT/config/"
cp schemas/*.json "$ROOT/schemas/"
cp -R docs/. "$ROOT/docs/"
cp scripts/smoke-test.sh scripts/inventory.sh "$ROOT/scripts/"
cp tools/verify_artifacts.py "$ROOT/tools/"
cp deploy/S99ahpv "$ROOT/deploy/"
cp -R tests/hardware "$ROOT/tests/"
[ -f tests/fixtures/generated/static.y4m ] && cp tests/fixtures/generated/static.y4m tests/fixtures/generated/corpus-manifest.json "$ROOT/fixtures/"
cp README.md VERSION "$ROOT/"
{
    echo "name: $NAME"
    echo "version: $VERSION"
    echo "build_id: $(make -s print-build-id)"
    echo "built_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "built_on: $(uname -a)"
    echo "compiler: $(${CXX:-c++} --version 2>&1 | head -n 1)"
    echo "target_os: $OS"
    echo "target_arch: $ARCH"
    echo "hardware_verified: no"
    echo "usage_notice: NOT FOR FLIGHT OR ENGAGEMENT CONTROL"
} > "$ROOT/BUILD-INFO.txt"
(cd "$ROOT" && find . -type f ! -name SHA256SUMS | sort | while read -r f; do
    if command -v sha256sum >/dev/null; then sha256sum "$f"; else shasum -a 256 "$f"; fi
done > SHA256SUMS)
mkdir -p releases
tar -C "$STAGE" -czf "releases/$NAME.tar.gz" "$NAME"
(cd releases && if command -v sha256sum >/dev/null; then sha256sum "$NAME.tar.gz"; else shasum -a 256 "$NAME.tar.gz"; fi > "$NAME.tar.gz.sha256")
echo "releases/$NAME.tar.gz"
