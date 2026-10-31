#!/bin/sh
# HWR-003 / AC-014 resource profile. Samples memory/CPU/storage for a labelled
# state (boot, camera_ready, preview, recording). Read-only.
# Usage: sh resource_profile.sh LABEL SECONDS INTERVAL DATA_DIR > profile-LABEL.csv
LABEL=${1:?label}; SECONDS_TOTAL=${2:-60}; INTERVAL=${3:-1}; DATA=${4:-/data}
echo "label,utc,uptime_s,mem_total_kb,mem_available_kb,cma_total_kb,cma_free_kb,cpu_total,cpu_idle,ahpv_rss_kb,ahpv_hwm_kb,data_free_kb,temp_mC"
end=$(( $(cut -d. -f1 /proc/uptime) + SECONDS_TOTAL ))
while [ "$(cut -d. -f1 /proc/uptime)" -lt "$end" ]; do
    m() { awk -v k="$1:" '$1==k {print $2}' /proc/meminfo; }
    set -- $(head -n1 /proc/stat)
    total=$(( $2 + $3 + $4 + $5 + $6 + $7 + $8 + $9 )); idle=$(( $5 + $6 ))
    pid=$(pidof ahpv 2>/dev/null | awk '{print $1}')
    rss=""; hwm=""
    [ -n "$pid" ] && rss=$(awk '/^VmRSS/ {print $2}' /proc/$pid/status) && hwm=$(awk '/^VmHWM/ {print $2}' /proc/$pid/status)
    free=$(df -k "$DATA" 2>/dev/null | awk 'NR==2 {print $4}')
    temp=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)
    echo "$LABEL,$(date -u +%Y-%m-%dT%H:%M:%SZ),$(cut -d' ' -f1 /proc/uptime),$(m MemTotal),$(m MemAvailable),$(m CmaTotal),$(m CmaFree),$total,$idle,$rss,$hwm,$free,$temp"
    sleep "$INTERVAL"
done
