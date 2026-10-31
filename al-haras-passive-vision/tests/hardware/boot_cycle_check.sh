#!/bin/sh
# AC-001 / NFR-001: run once per boot (e.g. from /etc/init.d/S99zz-bootcheck)
# after S99ahpv. Appends one record per boot: whether the APP-001 ready marker
# appeared in the service log within the deadline. Power cycling itself is a
# manual or relay-driven step recorded in the boot-cycle sheet.
LOG=${AHPV_LOG:-/data/ahpv/ahpv.log}; OUT=${AHPV_BOOTLOG:-/data/ahpv/boot-cycles.csv}; DEADLINE=${AHPV_READY_S:-60}
BOOT_ID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)
start=$(cut -d. -f1 /proc/uptime); ready=no
while [ $(( $(cut -d. -f1 /proc/uptime) - start )) -lt "$DEADLINE" ]; do
    # APP-001 carries the kernel boot_id, so a marker from an earlier boot never counts.
    if grep '"event_id":"APP-001"' "$LOG" 2>/dev/null | grep -q "\"boot_id\":\"$BOOT_ID\""; then ready=yes; break; fi
    sleep 1
done
[ -f "$OUT" ] || echo "boot_id,utc,uptime_s,ready" > "$OUT"
echo "$BOOT_ID,$(date -u +%Y-%m-%dT%H:%M:%SZ),$(cut -d' ' -f1 /proc/uptime),$ready" >> "$OUT"
sync
