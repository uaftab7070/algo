"""AC-020 / NFR-012 / SEC-002 / SEC-003 / SEC-006 audits on the host build.

Source, binary-import, process and socket evidence that no capture, annotation
or evaluation output can reach a vehicle, gimbal, serial or actuator interface.
The device-side audit (board processes, /dev nodes, serial configuration, network
scan) repeats these checks on hardware: tests/hardware/device_audit.sh.
"""
import os
import re
import signal
import subprocess
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from pyharness import BIN, ROOT, Env  # noqa: E402

SRC = os.path.join(ROOT, 'src')


def source_files():
    for base, _, names in os.walk(SRC):
        for name in names:
            if name.endswith(('.cpp', '.hpp', '.js', '.html')):
                yield os.path.join(base, name)


TOKENS = re.compile(r'R"AHPVUI\(.*?\)AHPVUI"|/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])+\'', re.S)


def strip_comments_and_strings(text):
    """Single pass, so '//' inside a string or quotes inside comments cannot misalign."""
    return TOKENS.sub(lambda m: '""' if m.group(0)[0] in 'R"\'' else ' ', text)


FORBIDDEN = re.compile(r'(?i)\b\w*(mavlink|sbus|crsf|dshot|msp_|pwm|setpoint|steer|aim_?point|intercept|pursuit|'
                       r'lock_?on|guidance|gimbal|actuator|servo|throttle|yaw_rate|autopilot|waypoint)\w*\b')


class SourceAudit(unittest.TestCase):
    def test_audit_detects_planted_identifiers(self):
        planted = 'int mavlink_port = 0; // gimbal note\nauto u = "http://x"; send_setpoint(1); s = "MAVLink";'
        found = [m.group(0) for m in FORBIDDEN.finditer(strip_comments_and_strings(planted))]
        self.assertEqual(found, ['mavlink_port', 'send_setpoint'])

    def test_no_control_vocabulary_in_code(self):
        forbidden = FORBIDDEN
        hits = []
        for path in source_files():
            code = strip_comments_and_strings(open(path, encoding='utf-8').read())
            hits += [f'{os.path.relpath(path, ROOT)}: {m.group(0)}' for m in forbidden.finditer(code)]
        self.assertEqual(hits, [])

    def test_no_serial_bus_or_outbound_network_apis(self):
        rules = {
            r'#include\s*<(termios\.h|linux/can\.h|linux/serial\.h|linux/i2c-dev\.h|linux/spi/spidev\.h|linux/gpio\.h|sys/serial\.h)>': None,
            r'\b(connect|sendto|sendmsg|tcsetattr|cfsetospeed)\s*\(': None,
            r'SOCK_DGRAM|AF_CAN|AF_PACKET|AF_BLUETOOTH': None,
            r'#include\s*<sys/ioctl\.h>': 'src/capture/v4l2_source.cpp',     # V4L2 capture ioctls only.
            r'\b(socket|bind|listen|accept)\s*\(': 'src/preview/http_server.cpp',  # The single declared listener.
        }
        violations = []
        for path in source_files():
            rel = os.path.relpath(path, ROOT)
            code = strip_comments_and_strings(open(path, encoding='utf-8').read())
            for pattern, allowed in rules.items():
                if re.search(pattern, code) and rel != allowed:
                    violations.append(f'{rel}: {pattern}')
        self.assertEqual(violations, [])

    def test_v4l2_adapter_only_issues_capture_ioctls(self):
        code = strip_comments_and_strings(open(os.path.join(SRC, 'capture', 'v4l2_source.cpp')).read())
        used = set(re.findall(r'VIDIOC_[A-Z_]+', code))
        self.assertTrue(used <= {'VIDIOC_QUERYCAP', 'VIDIOC_S_FMT', 'VIDIOC_S_PARM', 'VIDIOC_G_PARM', 'VIDIOC_REQBUFS',
                                 'VIDIOC_QUERYBUF', 'VIDIOC_QBUF', 'VIDIOC_DQBUF', 'VIDIOC_STREAMON', 'VIDIOC_STREAMOFF'}, used)

    def test_only_annotation_writes_over_http(self):
        code = open(os.path.join(SRC, 'preview', 'preview_service.cpp')).read()
        writes = set(re.findall(r'm == "(POST|PUT|DELETE|PATCH)"', code))
        self.assertEqual(writes, {'POST', 'PUT'})
        for line in re.findall(r'.*m == "(?:POST|PUT)".*', code):
            self.assertIn('annotation_write', line)


class BinaryAudit(unittest.TestCase):
    def test_binary_imports_no_outbound_or_serial_symbols(self):
        nm = subprocess.run(['nm', '-u', BIN], capture_output=True, text=True)
        if nm.returncode != 0:
            self.skipTest('nm unavailable')
        imported = {s.strip().lstrip('_') for s in nm.stdout.split()}
        self.assertTrue({'bind', 'listen', 'accept'} <= imported)
        self.assertEqual(imported & {'connect', 'sendto', 'sendmsg', 'tcsetattr', 'cfsetospeed', 'getaddrinfo', 'gethostbyname'}, set())


class RuntimeAudit(unittest.TestCase):
    def lsof(self, pid):
        return subprocess.run(['lsof', '-n', '-P', '-p', str(pid)], capture_output=True, text=True).stdout

    def test_sec003_only_declared_listener_and_no_device_handles(self):
        if subprocess.run(['which', 'lsof'], capture_output=True).returncode:
            self.skipTest('lsof unavailable')
        quiet = Env('quiet', extra={'source.realtime': 'true'})
        proc = quiet.start('--exit-on-end')
        time.sleep(1.0)
        listing = self.lsof(proc.pid)
        proc.send_signal(signal.SIGTERM)
        proc.communicate(timeout=20)
        self.assertNotIn('TCP', listing)  # Preview disabled by default: no listener at all.
        self.assertNotIn('UDP', listing)
        preview = Env('listen', extra={'source.realtime': 'true', 'preview.enabled': 'true'})
        proc = preview.start()
        try:
            preview.wait_http()
            listing = self.lsof(proc.pid)
        finally:
            proc.send_signal(signal.SIGTERM)
            proc.communicate(timeout=20)
        sockets = [l for l in listing.splitlines() if ' TCP ' in l or ' UDP ' in l]
        self.assertEqual(len([s for s in sockets if '(LISTEN)' in s]), 1, sockets)
        self.assertIn(f'127.0.0.1:{preview.port} (LISTEN)', '\n'.join(sockets))
        self.assertFalse([s for s in sockets if '->' in s and f':{preview.port}->' not in s], sockets)  # No outbound.
        self.assertFalse(re.search(r'/dev/(tty|cu\.|video|i2c|spi|gpio)', listing))


class SecretScan(unittest.TestCase):
    def test_sec002_no_embedded_credentials(self):
        patterns = [re.compile(p) for p in (r'ahpv_[0-9a-f]{64}', r'-----BEGIN [A-Z ]*PRIVATE KEY-----',
                                            r'(?i)(password|passwd|secret|api_key)\s*[:=]\s*["\'][^"\']{4,}')]
        hits = []
        for base, dirs, names in os.walk(ROOT):
            dirs[:] = [d for d in dirs if d not in ('build', '.git', 'releases')]
            for name in names:
                path = os.path.join(base, name)
                try:
                    text = open(path, encoding='utf-8').read()
                except (UnicodeDecodeError, OSError):
                    continue
                hits += [f'{os.path.relpath(path, ROOT)}: {p.pattern}' for p in patterns if p.search(text)]
        self.assertEqual(hits, [])


if __name__ == '__main__':
    unittest.main()
