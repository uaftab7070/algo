#!/bin/sh
# AC-013 / NFR-002 / NFR-003: 24 h capture-preview-record soak on the board.
# Requires the verified camera configuration and the service installed.
# Usage: sh soak.sh CONFIG TOKEN HOURS OUT_DIR
CONFIG=${1:?config}; TOKEN=${2:?token}; HOURS=${3:-24}; OUT=${4:-/data/ahpv-soak}
BIN=${AHPV_BIN:-/opt/ahpv/bin/ahpv}
mkdir -p "$OUT"
"$BIN" run --config "$CONFIG" --token-file "$TOKEN" --purpose "24h soak (AC-013)" --partition acceptance \
    --duration-s $(( HOURS * 3600 )) --exit-on-end > "$OUT/run.json" 2> "$OUT/stderr.log" &
PID=$!
sh "$(dirname "$0")/resource_profile.sh" soak $(( HOURS * 3600 + 60 )) 60 "$(dirname "$CONFIG")" > "$OUT/resources.csv" &
wait $PID; STATUS=$?
echo "exit_status=$STATUS" > "$OUT/result.txt"
for sid in $(sed -n 's/.*"sessions":\[\(.*\)\].*/\1/p' "$OUT/run.json" | tr -d '"' | tr ',' ' '); do
    "$BIN" verify --config "$CONFIG" "$sid" > "$OUT/verify-$sid.json"; echo "$sid verify=$?" >> "$OUT/result.txt"
done
grep -c '"level":"error"' "$OUT/stderr.log" >> "$OUT/result.txt"
