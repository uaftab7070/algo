#!/bin/sh
# Read-only board inventory for HWR-001..HWR-005 (no Python, no binary needed).
# It reads files and lists device names only. It does not start capture, probe
# buses, open serial ports, change configuration, mount, flash or install.
# Usage on the board: sh inventory.sh > inventory-$(date +%Y%m%d).txt
section() { printf '\n===== %s =====\n' "$1"; }
show() { [ -r "$1" ] && { section "$1"; cat "$1" 2>/dev/null | tr '\000' ' '; echo; }; }
section "collected"; date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null; echo "collector: al-haras-passive-vision scripts/inventory.sh"
section "uname"; uname -a
show /etc/os-release
show /etc/version
show /proc/device-tree/model
show /proc/device-tree/compatible
show /proc/device-tree/serial-number
show /proc/cpuinfo
show /proc/meminfo
show /proc/cmdline
show /proc/mtd
show /proc/partitions
show /proc/mounts
section "df"; df -k 2>/dev/null
section "video4linux"
for d in /sys/class/video4linux/*; do [ -e "$d/name" ] && printf '%s: %s\n' "${d##*/}" "$(cat "$d/name")"; done
section "media/video/npu/rga nodes"; ls -l /dev/video* /dev/media* /dev/v4l-subdev* /dev/rknpu* /dev/rga* /dev/mpp_service 2>/dev/null
section "camera sensor drivers (dmesg)"; dmesg 2>/dev/null | grep -iE 'mis5001|sc3336|sc4336|rkisp|rkcif|csi|mipi' | tail -n 80
section "kernel modules"; cat /proc/modules 2>/dev/null
section "vendor libraries (names only; not ABI verification)"
ls -l /oem/usr/lib /usr/lib 2>/dev/null | grep -iE 'rkaiq|rockit|rkmpi|easymedia|rga|rknn|mpp' 
section "processes"; ps 2>/dev/null || ps -ef 2>/dev/null
section "listening sockets"; (netstat -ltnup 2>/dev/null || netstat -an 2>/dev/null | grep -i listen)
show /proc/net/tcp
show /proc/net/udp
section "network interfaces"; ls /sys/class/net 2>/dev/null
section "serial device names (not opened)"; ls /dev/ttyS* /dev/ttyFIQ* /dev/ttyGS* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
section "usb devices"; for d in /sys/bus/usb/devices/*; do [ -e "$d/product" ] && printf '%s: %s\n' "${d##*/}" "$(cat "$d/product")"; done
section "thermal"; for z in /sys/class/thermal/thermal_zone*; do [ -e "$z/temp" ] && printf '%s %s %s\n' "${z##*/}" "$(cat "$z/type")" "$(cat "$z/temp")"; done
section "cma"; grep -i cma /proc/meminfo 2>/dev/null; dmesg 2>/dev/null | grep -i cma | tail -n 10
section "init scripts"; ls -l /etc/init.d 2>/dev/null
section "end"
