#!/bin/sh
# Runs every host test suite and captures acceptance evidence.
#   scripts/test-report.sh [EVIDENCE_DIR]
# Produces EVIDENCE_DIR (default evidence/host-<UTC>) containing environment.txt,
# one log per suite, sample session/report artifacts and summary.json.
# Host evidence only: it does not substitute for tests on the Luckfox Pico Max.
set -u
cd "$(dirname "$0")/.."
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT=${1:-evidence/host-$STAMP}
mkdir -p "$OUT"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/ahpv-evidence-XXXXXX")
trap 'rm -rf "$WORK"' EXIT
PY="python3 -I -W ignore"

{
    echo "collected_utc: $STAMP"
    echo "host: $(uname -a)"
    command -v sw_vers >/dev/null && sw_vers
    echo "compiler: $(c++ --version 2>&1 | head -n 1)"
    echo "python: $(python3 --version 2>&1)"
    echo "node: $(node --version 2>/dev/null || echo unavailable)"
    echo "target_hardware: NONE (development host; Luckfox Pico Max not attached)"
} > "$OUT/environment.txt"

echo "== build" | tee "$OUT/build.log"
make -j4 >> "$OUT/build.log" 2>&1 && make -j4 sanitize >> "$OUT/build.log" 2>&1
BUILD_STATUS=$?
{
    echo "version: $(build/host/ahpv version)"
    echo "binary_sha256: $(shasum -a 256 build/host/ahpv 2>/dev/null || sha256sum build/host/ahpv)"
} >> "$OUT/environment.txt"
$PY tests/fixtures/make_fixtures.py tests/fixtures/generated

status_json=""
run_suite() {  # name, command...
    name=$1; shift
    echo "== $name"
    start=$(date +%s)
    "$@" > "$OUT/$name.log" 2>&1
    code=$?
    echo "   exit=$code ($(( $(date +%s) - start ))s)"
    status_json="$status_json\"$name\":{\"exit\":$code,\"log\":\"$name.log\"},"
}

mkdir -p "$WORK/unit" "$WORK/unit-asan" "$WORK/it" "$WORK/it-asan" "$WORK/faults" "$WORK/acceptance"
run_suite unit env AHPV_TEST_TMP="$WORK/unit" build/host/ahpv-unit-tests
run_suite unit-asan env AHPV_TEST_TMP="$WORK/unit-asan" build/asan/ahpv-unit-tests
run_suite integration env AHPV_TEST_TMP="$WORK/it" $PY tests/integration/test_recording_replay.py -v
run_suite integration-asan env AHPV_BIN=build/asan/ahpv AHPV_TEST_TMP="$WORK/it-asan" $PY tests/integration/test_recording_replay.py -v
run_suite faults env AHPV_TEST_TMP="$WORK/faults" $PY tests/faults/test_faults.py -v
run_suite faults-asan env AHPV_BIN=build/asan/ahpv AHPV_TEST_TMP="$WORK/faults" $PY tests/faults/test_faults.py -v
run_suite acceptance env AHPV_TEST_TMP="$WORK/acceptance" $PY tests/acceptance/test_no_control_path.py -v
run_suite ui-syntax node --check src/preview/ui/app.js
run_suite smoke env TMPDIR="$WORK" sh scripts/smoke-test.sh

# Sample artifacts for reviewers: a fault-injected session, its report and verification.
SAMPLE="$OUT/samples"
mkdir -p "$SAMPLE"
cat > "$WORK/sample.yaml" <<CFG
data_root: "$WORK/sample-data"
source:
  kind: "y4m_file"
  path: "$(pwd)/tests/fixtures/generated/illumination.y4m"
storage:
  warn_free_bytes: "1048576"
  reserve_free_bytes: "524288"
metrics:
  sample_interval_ms: "100"
logging:
  level: "warn"
CFG
build/host/ahpv auth add --config "$WORK/sample.yaml" --principal evidence-operator --role operator \
    | sed -n 's/.*"token":"\([^"]*\)".*/\1/p' > "$WORK/op.token"
chmod 600 "$WORK/op.token"
build/host/ahpv run --config "$WORK/sample.yaml" --token-file "$WORK/op.token" --purpose "evidence sample" \
    --partition development --place-category "synthetic fixture" --owner "test harness" --permission-record "generated data" \
    --scene-label "illumination ramp" --fault-injection "drop=10;corrupt=20;delay=40:150" --exit-on-end > "$SAMPLE/run.json" 2> "$SAMPLE/run-stderr.log"
SID=$(sed -n 's/.*"sessions":\["\([^"]*\)".*/\1/p' "$SAMPLE/run.json")
if [ -n "$SID" ]; then
    build/host/ahpv verify --config "$WORK/sample.yaml" "$SID" > "$SAMPLE/verify.json"
    build/host/ahpv report --config "$WORK/sample.yaml" "$SID" --contact-sheet 9 > "$SAMPLE/report-bundle.json"
    SESSION_DIR="$WORK/sample-data/sessions/$SID"
    mkdir -p "$SAMPLE/session"
    for f in session.json frames.jsonl events.jsonl metrics.jsonl checksums.json; do cp "$SESSION_DIR/$f" "$SAMPLE/session/"; done
    cp -R "$SESSION_DIR/reports" "$SAMPLE/session/reports"
    $PY tools/verify_artifacts.py session "$SESSION_DIR" > "$SAMPLE/independent-verify-session.json"
    $PY tools/verify_artifacts.py report "$(ls -d "$SESSION_DIR"/reports/*/ | head -n 1)" > "$SAMPLE/independent-verify-report.json"
    $PY tools/verify_artifacts.py audit "$WORK/sample-data/audit.jsonl" > "$SAMPLE/independent-verify-audit.json"
    cp "$WORK/sample-data/audit.jsonl" "$SAMPLE/audit.jsonl"
fi
build/host/ahpv diag --config "$WORK/sample.yaml" > "$SAMPLE/diag.json" 2>/dev/null
build/host/ahpv inventory > "$SAMPLE/host-inventory.json"

printf '{"collected_utc":"%s","build_exit":%s,"suites":{%s},"target_hardware":"none"}\n' \
    "$STAMP" "$BUILD_STATUS" "${status_json%,}" > "$OUT/summary.json"
echo "evidence written to $OUT"
grep -q '"exit":[1-9]' "$OUT/summary.json" && exit 1
[ "$BUILD_STATUS" -eq 0 ]
