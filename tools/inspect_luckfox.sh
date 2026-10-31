#!/bin/sh
# Read-only inventory. Run on the Luckfox; no root, streaming, or UART writes.
set -u

if [ "$(uname -s)" != Linux ]; then
    printf '%s\n' 'Run this script on the Linux Luckfox board, not the workstation.' >&2
    exit 2
fi

section() { printf '\n%s\n' "$1"; }
show_file() {
    if [ -r "$1" ]; then
        printf '%s\n' "$1"
        cat "$1"
        printf '\n'
    fi
}

section 'Kernel and architecture'
uname -srm
section 'Board model (device tree)'
if [ -r /proc/device-tree/model ]; then
    tr '\000' '\n' < /proc/device-tree/model
else
    printf '%s\n' 'Device-tree model unavailable'
fi
section 'OS image'
show_file /etc/os-release
show_file /etc/version
section 'Memory (kernel-visible, not physical capacity)'
if [ -r /proc/meminfo ]; then
    sed -n '/^MemTotal:/p; /^MemAvailable:/p; /^CmaTotal:/p; /^CmaFree:/p' /proc/meminfo
fi
section 'Video and media device names (does not start capture)'
for device in /sys/class/video4linux/* /sys/class/media/*; do
    [ -d "$device" ] || continue
    printf '%s\n' "$device"
    show_file "$device/name"
done
section 'I2C device names (existing sysfs entries; no bus probing)'
for device in /sys/bus/i2c/devices/*/name; do
    [ -r "$device" ] || continue
    show_file "$device"
done
section 'Common Rockchip runtime library locations'
for base in /lib /usr/lib /usr/local/lib /oem/usr/lib /oem/lib; do
    for library in "$base"/librknn* "$base"/librkmpi* "$base"/librkaiq* "$base"/librga*; do
        [ -e "$library" ] || continue
        ls -l "$library"
    done
done
section 'Available inspection tools'
for tool in media-ctl v4l2-ctl rkipc python3; do
    command -v "$tool" || printf '%s not found\n' "$tool"
done
section 'UART device names only (no port is opened)'
for device in /dev/ttyS* /dev/ttyAMA* /dev/ttyUSB* /dev/ttyACM*; do
    [ -e "$device" ] || continue
    printf '%s\n' "$device"
done
section 'Inventory complete; absence from these paths does not prove a library is missing.'
