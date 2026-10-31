#!/bin/sh
# End-to-end smoke test (POSIX sh; runs on the host or the board, no Python).
# Records a fixture, verifies it, writes a report and checks the audit chain.
# Usage: scripts/smoke-test.sh [path/to/ahpv] [path/to/fixture.y4m]
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
BIN=${1:-$HERE/build/host/ahpv}
FIXTURE=${2:-}
if [ -z "$FIXTURE" ]; then
    for candidate in "$HERE/fixtures/static.y4m" "$HERE/tests/fixtures/generated/static.y4m"; do
        [ -r "$candidate" ] && FIXTURE=$candidate && break
    done
fi
[ -x "$BIN" ] || { echo "smoke: binary not found: $BIN" >&2; exit 2; }
[ -r "$FIXTURE" ] || { echo "smoke: fixture not found: $FIXTURE (generate with tests/fixtures/make_fixtures.py)" >&2; exit 2; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/ahpv-smoke-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
fail() { echo "SMOKE FAIL: $1" >&2; exit 1; }
cat > "$WORK/config.yaml" <<CFG
data_root: "$WORK/data"
source:
  kind: "y4m_file"
  path: "$FIXTURE"
storage:
  warn_free_bytes: "1048576"
  reserve_free_bytes: "524288"
logging:
  level: "warn"
CFG
"$BIN" version >/dev/null || fail "version"
"$BIN" config check --config "$WORK/config.yaml" >/dev/null || fail "config check"
"$BIN" auth add --config "$WORK/config.yaml" --principal smoke-operator --role operator > "$WORK/auth.json" || fail "auth add"
sed -n 's/.*"token":"\([^"]*\)".*/\1/p' "$WORK/auth.json" > "$WORK/op.token"
chmod 600 "$WORK/op.token"
OUT=$("$BIN" run --config "$WORK/config.yaml" --token-file "$WORK/op.token" --purpose smoke-test --partition development --exit-on-end) || fail "run"
SID=$(printf '%s\n' "$OUT" | sed -n 's/.*"sessions":\["\([^"]*\)".*/\1/p' | tail -n 1)
[ -n "$SID" ] || fail "no session created"
"$BIN" verify --config "$WORK/config.yaml" "$SID" > "$WORK/verify.json" || fail "verify ($(cat "$WORK/verify.json"))"
grep -q '"ok":true' "$WORK/verify.json" || fail "verify not ok"
"$BIN" report --config "$WORK/config.yaml" "$SID" > "$WORK/report.json" || fail "report"
"$BIN" audit-verify --config "$WORK/config.yaml" > /dev/null || fail "audit chain"
if "$BIN" config set --config "$WORK/config.yaml" preview.port 9000 2>/dev/null; then fail "unauthenticated config change accepted"; fi
echo "SMOKE PASS session=$SID"
