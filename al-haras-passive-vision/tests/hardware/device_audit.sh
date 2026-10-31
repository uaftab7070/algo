#!/bin/sh
# AC-020 / SEC-003 / SEC-006 device-side audit. Run on the board while the
# service is running. Read-only. Usage: sh device_audit.sh > device-audit.txt
PID=$(pidof ahpv 2>/dev/null | awk '{print $1}')
echo "audit_at: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "ahpv_pid: ${PID:-not running}"
echo "== listening sockets (all processes)"; netstat -ltnup 2>/dev/null || cat /proc/net/tcp /proc/net/udp
if [ -n "$PID" ]; then
    echo "== ahpv open descriptors"; ls -l "/proc/$PID/fd" 2>/dev/null
    echo "== ahpv descriptors on serial/bus/gpio nodes (expected: none)"
    ls -l "/proc/$PID/fd" 2>/dev/null | grep -E '/dev/(tty|i2c|spi|gpio|can|pwm)' || echo "none"
    echo "== ahpv sockets (expected: one 127.0.0.1 listener when preview is enabled, accepted local peers only)"
    ls -l "/proc/$PID/fd" 2>/dev/null | grep socket || echo "none"
    echo "== ahpv user"; grep -E '^(Uid|Gid)' "/proc/$PID/status"
    echo "== ahpv memory"; grep -E '^(VmRSS|VmHWM|VmSize|Threads)' "/proc/$PID/status"
fi
echo "== processes"; ps 2>/dev/null
echo "== serial device names (not opened)"; ls /dev/ttyS* /dev/ttyFIQ* /dev/ttyGS* 2>/dev/null
echo "== end"
