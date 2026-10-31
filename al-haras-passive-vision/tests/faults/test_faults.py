"""Fault-matrix tests (brief section 10) on the host build.

Hardware-only faults (physical camera unplug, power cut, board memory pressure)
are BLOCKED here; these tests exercise the same code paths through files,
FIFOs, injected faults and process termination.
"""
import json
import os
import signal
import subprocess
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from pyharness import NOTICE, Env, fixtures, verify_independently  # noqa: E402

W, H = 160, 120
FRAME_BYTES = W * H + 2 * (W // 2) * (H // 2)


def events(env, sid):
    return [e['event'] for e in env.lines(sid, 'events.jsonl')]


class InjectedFrameFaults(unittest.TestCase):
    def test_fr008_drop_corrupt_delay_are_flagged_and_accounted(self):
        env = Env('fr008')
        run = env.record('--fault-injection', 'drop=10,11;corrupt=20;delay=30:200')
        self.assertEqual(run.code, 0, run.stderr)
        [sid] = run.json['sessions']
        frames = {f['frame_id']: f for f in env.lines(sid, 'frames.jsonl')}
        evs = env.lines(sid, 'events.jsonl')
        # Dropped upstream: the next delivered frame reports the gap and its size.
        gap = [f for f in frames.values() if 'sequence_gap' in f['flags']]
        self.assertEqual(len(gap), 1)
        self.assertEqual((gap[0]['source_sequence'], gap[0]['missing_before']), (12, 2))
        rejected = [e for e in evs if e['event'] == 'rejected_frame']
        self.assertEqual(len(rejected), 1)
        self.assertTrue(rejected[0]['fault_injected'])
        self.assertNotIn(rejected[0]['frame_id'], frames)  # Never stored as valid input.
        # Late: the frame after the upstream gap (3-period interval) and the stalled frame.
        late = sorted((f for f in frames.values() if 'late' in f['flags']), key=lambda f: f['frame_id'])
        self.assertEqual([f['source_sequence'] for f in late], [12, 30])
        self.assertIn('fault_injected', late[1]['flags'])
        manifest = env.manifest(sid)
        self.assertEqual(manifest['fault_injection'], 'drop=10,11;corrupt=20;delay=30:200')
        self.assertTrue(env.cli('verify', sid, token=False).json['ok'])
        self.assertEqual(verify_independently('session', env.session_dir(sid))[0], 0)
        report = env.cli('report', sid, token=False).json
        with open(os.path.join(report['report_dir'], 'qa-report.json')) as f:
            det = json.load(f)['deterministic']
        self.assertEqual((det['frames']['missing_upstream'], det['frames']['rejected']), (2, 1))

    def test_memory_allocation_failure_stops_safely(self):
        env = Env('memfail')
        run = env.record('--fault-injection', 'alloc_fail=15')
        self.assertEqual(run.code, 5, run.stderr)
        [sid] = run.json['sessions']
        self.assertEqual(env.manifest(sid)['status'], 'error')
        self.assertIn('memory_failure', events(env, sid))
        self.assertTrue(env.cli('verify', sid, token=False).json['ok'])


class CorruptInputs(unittest.TestCase):
    def test_ac011_malformed_truncated_and_unsupported_inputs(self):
        for name in ('corrupt_header.y4m', 'truncated.y4m'):
            env = Env('ac011', source=name)
            run = env.record()
            self.assertEqual(run.code, 4, (name, run.stderr))
            [sid] = run.json['sessions']
            manifest = env.manifest(sid)
            self.assertEqual(manifest['status'], 'source_error')
            self.assertIn('source_error', events(env, sid))
            verify = env.cli('verify', sid, token=False).json
            self.assertTrue(verify['ok'], (name, verify))  # Frames before the fault remain valid.
            self.assertEqual(verify['summary']['frames_stored'], 6 if name == 'corrupt_header.y4m' else 8)
        for name in ('unsupported_422.y4m', 'interlaced.y4m', 'not_video.bin', 'empty_file.y4m', 'missing.y4m'):
            env = Env('ac011u', source=name)
            run = env.record()
            self.assertEqual(run.code, 3, (name, run.stderr))
            self.assertIn('initialization failed', run.stderr)
            self.assertEqual(env.sessions(), [], name)  # No fabricated session.
            # The application remains usable afterwards.
            env.write_config('y4m_file', {'source.path': os.path.join(fixtures(), 'static.y4m')})
            self.assertEqual(env.record().code, 0)


class CameraLoss(unittest.TestCase):
    def test_ac003_disconnect_without_reopen_is_terminal_and_explicit(self):
        env = Env('ac003')
        run = env.record('--fault-injection', 'disconnect=40')
        self.assertEqual(run.code, 3, run.stderr)
        [sid] = run.json['sessions']
        manifest = env.manifest(sid)
        self.assertEqual(manifest['status'], 'camera_lost')
        self.assertEqual(manifest['statistics']['frames_stored'], 40)
        self.assertIn('source_disconnected', events(env, sid))
        self.assertTrue(env.cli('verify', sid, token=False).json['ok'])

    def test_fr016_reopen_marks_discontinuity(self):
        env = Env('fr016', extra={'source.auto_reopen': 'true', 'source.reopen_delay_ms': '20'})
        run = env.record('--fault-injection', 'disconnect=40')
        self.assertEqual(run.code, 0, run.stderr)
        [sid] = run.json['sessions']
        frames = env.lines(sid, 'frames.jsonl')
        self.assertEqual(len(frames), 90)
        after = [f for f in frames if 'after_discontinuity' in f['flags']]
        self.assertEqual([(f['frame_id'], f['source_epoch']) for f in after], [(40, 1)])
        self.assertEqual(events(env, sid).count('source_reopened'), 1)


class LiveStream(unittest.TestCase):
    """A FIFO stands in for a live camera stream (delivery timestamps)."""

    def feed(self, fifo, frames, then_stall_s, close=True):
        with open(os.path.join(fixtures(), 'bench_motion.y4m'), 'rb') as f:
            data = f.read()
        header_end = data.index(b'\n') + 1
        with open(fifo, 'wb') as out:
            out.write(data[:header_end])
            for i in range(frames):
                start = header_end + i * (6 + FRAME_BYTES)
                out.write(data[start:start + 6 + FRAME_BYTES])
                out.flush()
                time.sleep(1 / 30)
            time.sleep(then_stall_s)
            if not close:
                time.sleep(3600)

    def test_ac003_sec007_no_signal_and_loss_never_show_stale_frames(self):
        env = Env('live', kind='y4m_stream', source='stream.fifo',
                  extra={'preview.enabled': 'true', 'source.no_signal_timeout_ms': '400'})
        os.mkfifo(env.source)
        writer = threading.Thread(target=self.feed, args=(env.source, 45, 1.5), daemon=True)
        writer.start()
        proc = env.start()
        try:
            env.wait_http()
            time.sleep(0.6)
            status, headers, body = env.http('GET', '/api/live/frame.bmp', token='view')
            self.assertEqual(status, 200, body)
            self.assertEqual(body[:2], b'BM')
            self.assertEqual(headers['X-AHPV-Usage-Notice'], NOTICE)
            time.sleep(1.5 + 0.4)  # Writer stalls: no-signal must be reported, not the last image.
            status, _, body = env.http('GET', '/api/live/frame.bmp', token='view')
            self.assertEqual(status, 503)
            self.assertIn(json.loads(body)['state'], ('no_signal', 'camera_lost'))
            writer.join(timeout=5)
            time.sleep(0.8)  # Writer closed the FIFO: camera lost.
            status, _, body = env.http('GET', '/api/status', token='view')
            self.assertEqual(json.loads(body)['state'], 'camera_lost')
            self.assertEqual(env.http('GET', '/api/live/frame.bmp', token='view')[0], 503)
        finally:
            proc.send_signal(signal.SIGTERM)
            out, err = proc.communicate(timeout=20)
        self.assertEqual(proc.returncode, 3, err)
        [sid] = env.sessions()
        manifest = env.manifest(sid)
        self.assertEqual((manifest['status'], manifest['source']['timestamp_basis']), ('camera_lost', 'stream_delivery_monotonic'))
        evs = events(env, sid)
        self.assertIn('no_signal', evs)
        self.assertIn('source_disconnected', evs)
        self.assertEqual(len(env.lines(sid, 'frames.jsonl')), 45)
        self.assertTrue(env.cli('verify', sid, token=False).json['ok'])

    def test_camera_absent_at_boot(self):
        env = Env('absent', kind='y4m_stream', source='never.fifo', extra={'source.no_signal_timeout_ms': '300'})
        os.mkfifo(env.source)
        run = env.record()
        self.assertEqual(run.code, 3)
        self.assertIn('camera stream absent', run.stderr)
        self.assertEqual(env.sessions(), [])


class StorageFaults(unittest.TestCase):
    def test_ac015_session_byte_limit(self):
        env = Env('ac015', extra={'recording.max_session_bytes': '1200000'})
        run = env.record()
        self.assertEqual(run.code, 0, run.stderr)
        [sid] = run.json['sessions']
        manifest = env.manifest(sid)
        self.assertEqual(manifest['status'], 'stopped_storage')
        self.assertLess(manifest['statistics']['frames_stored'], 90)
        self.assertTrue(env.cli('verify', sid, token=False).json['ok'])

    def test_ac015_real_filesystem_exhaustion(self):
        """Uses a small macOS disk image so writes genuinely fail with ENOSPC."""
        if sys.platform != 'darwin' or subprocess.run(['which', 'hdiutil'], capture_output=True).returncode:
            self.skipTest('hdiutil unavailable: real-volume exhaustion runs on the board (tests/hardware)')
        env = Env('enospc')
        image, mount = os.path.join(env.root, 'small.dmg'), os.path.join(env.root, 'vol')
        os.mkdir(mount)
        created = subprocess.run(['hdiutil', 'create', '-size', '3m', '-fs', 'HFS+', '-volname', 'ahpvfull', image],
                                 capture_output=True, text=True)
        attached = created.returncode == 0 and subprocess.run(
            ['hdiutil', 'attach', '-nobrowse', '-mountpoint', mount, image], capture_output=True, text=True).returncode == 0
        if not attached:
            self.skipTest('could not create/attach a disk image in this environment')
        try:
            data = os.path.join(mount, 'data')
            # A first, complete session that must survive the later exhaustion.
            env.write_config('y4m_file', {'data_root': data, 'source.path': os.path.join(fixtures(), 'static.y4m'),
                                          'security.auth_file': os.path.join(env.data, 'auth.conf'),
                                          'security.audit_log': os.path.join(env.root, 'audit.jsonl'),
                                          'storage.warn_free_bytes': '0', 'storage.reserve_free_bytes': '0',
                                          'recording.max_session_bytes': '500000'})
            first = env.record()
            self.assertEqual(first.code, 0, first.stderr)
            env.write_config('y4m_file', {'data_root': data, 'source.path': os.path.join(fixtures(), 'bench_motion.y4m'),
                                          'security.auth_file': os.path.join(env.data, 'auth.conf'),
                                          'security.audit_log': os.path.join(env.root, 'audit.jsonl'),
                                          'storage.warn_free_bytes': '0', 'storage.reserve_free_bytes': '0'})
            second = env.record()
            [sid1], [sid2] = first.json['sessions'], second.json['sessions']
            m2 = json.load(open(os.path.join(data, 'sessions', sid2, 'session.json')))
            self.assertIn(m2['status'], ('stopped_storage', 'recording'), m2.get('stop_reason'))
            if m2['status'] == 'recording':  # Final manifest itself could not be written: recovery path.
                env.write_config('y4m_file', {'data_root': data, 'source.path': os.path.join(fixtures(), 'static.y4m'),
                                              'security.auth_file': os.path.join(env.data, 'auth.conf'),
                                              'security.audit_log': os.path.join(env.root, 'audit.jsonl')})
            v1 = subprocess.run([env_bin(), 'verify', sid1, '--config', env.config], capture_output=True, text=True)
            self.assertEqual(v1.returncode, 0, v1.stdout)  # Existing session unharmed.
        finally:
            subprocess.run(['hdiutil', 'detach', mount, '-force'], capture_output=True)


def env_bin():
    from pyharness import BIN
    return BIN


class AbruptTermination(unittest.TestCase):
    def test_ac017_ac021_sigkill_then_recover(self):
        env = Env('ac021', extra={'source.realtime': 'true', 'recording.fsync_interval_frames': '3'})
        proc = env.start('--exit-on-end')
        time.sleep(1.5)
        proc.send_signal(signal.SIGKILL)
        proc.communicate(timeout=10)
        [sid] = env.sessions()
        self.assertEqual(env.manifest(sid)['status'], 'recording')
        verify = env.cli('verify', sid, token=False)
        self.assertEqual(verify.code, 1)  # Unfinalized sessions are not reported as valid.
        recover = env.cli('recover', sid)
        self.assertTrue(recover.json['recovered'], recover.stdout + recover.stderr)
        manifest = env.manifest(sid)
        self.assertEqual(manifest['status'], 'interrupted')
        stored = manifest['recovery']['indexed_frames']
        self.assertTrue(10 < stored < 90, stored)
        self.assertEqual(manifest['recovery']['tail_crc_failures'], 0)
        verify = env.cli('verify', sid, token=False).json
        self.assertTrue(verify['ok'], verify)
        self.assertEqual(verify_independently('session', env.session_dir(sid))[0], 0)

    def test_startup_recovers_interrupted_sessions_automatically(self):
        env = Env('autorecover', extra={'source.realtime': 'true'})
        proc = env.start('--exit-on-end')
        time.sleep(1.0)
        proc.send_signal(signal.SIGKILL)
        proc.communicate(timeout=10)
        [interrupted] = env.sessions()
        env.write_config('y4m_file', {'source.path': os.path.join(fixtures(), 'static.y4m')})
        run = env.record()
        self.assertEqual(run.code, 0)
        self.assertIn('startup_recovery', run.stdout)
        self.assertEqual(env.manifest(interrupted)['status'], 'interrupted')

    def test_sigterm_finalizes_cleanly(self):
        env = Env('sigterm', extra={'source.realtime': 'true'})
        proc = env.start('--exit-on-end')
        time.sleep(1.0)
        proc.send_signal(signal.SIGTERM)
        proc.communicate(timeout=20)
        self.assertEqual(proc.returncode, 0)
        [sid] = env.sessions()
        self.assertEqual(env.manifest(sid)['status'], 'complete')
        self.assertIn('operator_stop', events(env, sid))
        self.assertTrue(env.cli('verify', sid, token=False).json['ok'])


class ConfigurationAndAuthorization(unittest.TestCase):
    def test_nfr008_fr013_invalid_configuration_is_actionable(self):
        env = Env('cfg')
        with open(env.config, 'a') as f:
            f.write('flight_controller: "/dev/ttyS3"\n')
        result = env.cli('config', 'check', token=False)
        self.assertEqual(result.code, 2)
        self.assertRegex(result.stderr, r"line \d+: unknown configuration key 'flight_controller'")

    def test_ac019_unauthorized_configuration_changes_are_denied_and_audited(self):
        env = Env('ac019')
        before = open(env.config).read()
        for token in ('ann', 'view', 'rev'):
            r = env.cli('config', 'set', 'preview.port', '9001', token=token)
            self.assertEqual(r.code, 6, r.stderr)
        no_token = subprocess.run([env_bin(), 'config', 'set', 'preview.port', '9001', '--config', env.config],
                                  capture_output=True, text=True, env={k: v for k, v in os.environ.items() if k != 'AHPV_TOKEN'})
        self.assertEqual(no_token.returncode, 6)
        self.assertEqual(open(env.config).read(), before)
        self.assertEqual(env.cli('run', '--purpose', 'x', '--partition', 'development', token='view').code, 6)
        denied = [a for a in env.audit() if a['outcome'] == 'denied']
        self.assertGreaterEqual(len(denied), 5)
        ok = env.cli('config', 'set', 'preview.port', '9001', token='op')
        self.assertEqual(ok.code, 0, ok.stderr)
        self.assertTrue(os.path.exists(ok.json['previous_version']))
        self.assertEqual(open(ok.json['previous_version']).read(), before)
        self.assertTrue(env.cli('config', 'check', token=False).json['effective']['preview']['port'] == 9001)
        audit = subprocess.run([sys.executable, '-I', os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__)))), 'tools', 'verify_artifacts.py'), 'audit', os.path.join(env.data, 'audit.jsonl')],
            capture_output=True, text=True)
        self.assertEqual(audit.returncode, 0, audit.stdout)

    def test_auth_file_permissions_are_enforced(self):
        env = Env('authperm')
        os.chmod(os.path.join(env.data, 'auth.conf'), 0o644)
        r = env.record()
        self.assertEqual(r.code, 6)
        self.assertIn('chmod 600', r.stderr)


class PreviewApiSecurity(unittest.TestCase):
    def setUp(self):
        self.env = Env('api', source='static.y4m')
        [self.sid] = self.env.record().json['sessions']
        self.proc = subprocess.Popen([env_bin(), 'serve', '--config', self.env.config], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.env.wait_http()

    def tearDown(self):
        self.proc.send_signal(signal.SIGTERM)
        self.proc.communicate(timeout=10)

    def test_sec001_sec005_ac022_api_access_and_validation(self):
        env, sid = self.env, self.sid
        self.assertEqual(env.http('GET', '/api/sessions')[0], 401)
        self.assertEqual(env.http('GET', '/api/sessions', token='view')[0], 200)
        self.assertEqual(env.http('GET', '/api/sessions', token='view', host='evil.example:80')[0], 421)
        body = {'frame_id': 3, 'region_xywh_pixels': [5, 5, 20, 20], 'notes': '<img src=x onerror=alert(1)>'}
        self.assertEqual(env.http('POST', f'/api/sessions/{sid}/annotations', token='ann', body=body, origin='http://evil.example')[0], 403)
        self.assertEqual(env.http('POST', f'/api/sessions/{sid}/annotations', token='view', body=body)[0], 403)
        status, _, created = env.http('POST', f'/api/sessions/{sid}/annotations', token='ann', body=body)
        self.assertEqual(status, 201, created)
        record = json.loads(created)
        self.assertEqual(record['notes'], body['notes'])  # Stored as inert text; the UI renders with textContent.
        for bad in ({'frame_id': 3, 'region_xywh_pixels': [150, 5, 20, 20]},
                    {'frame_id': 3, 'region_xywh_pixels': [5, 5, 20, 20], 'aim_offset': [1, 1]},
                    {'frame_id': 3, 'region_xywh_pixels': [5, 5, 20, 20], 'command': 'follow'}):
            status, _, data = env.http('POST', f'/api/sessions/{sid}/annotations', token='ann', body=bad)
            self.assertEqual(status, 400, data)
        self.assertEqual(env.http('POST', f'/api/sessions/{sid}/annotations', token='ann', body=b'{bad json')[0], 400)
        current = json.loads(env.http('GET', f'/api/sessions/{sid}/annotations', token='view')[2])['annotations']
        self.assertEqual(len(current), 1)  # Rejected writes changed nothing.
        for method, path in (('POST', '/api/config'), ('PUT', '/api/control/start'), ('POST', '/api/capture')):
            self.assertEqual(env.http(method, path, token='op', body={})[0], 403)
        status, headers, image = env.http('GET', f'/api/sessions/{sid}/frames/3.bmp', token='view')
        self.assertEqual((status, image[:2]), (200, b'BM'))
        self.assertIn("default-src 'none'", headers['Content-Security-Policy'])
        self.assertEqual(env.http('GET', f'/api/sessions/{sid}/frames/999.bmp', token='view')[0], 404)
        self.assertEqual(env.http('GET', '/api/sessions/..%2f..%2fetc', token='view')[0], 400)
        self.assertEqual(env.http('GET', '/api/live/frame.bmp', token='view')[0], 404)  # Review-only mode.
        outcomes = {(a['principal'], a['action'].split(' ')[0], a['outcome']) for a in env.audit()}
        self.assertIn(('unauthenticated', 'GET', 'denied'), outcomes)
        self.assertIn(('view', 'annotation.create', 'denied'), outcomes)
        self.assertIn(('op', 'POST', 'denied'), outcomes)

    def test_frame_integrity_failure_is_not_displayed(self):
        env, sid = self.env, self.sid
        frame = env.lines(sid, 'frames.jsonl')[4]
        with open(os.path.join(env.session_dir(sid), 'video.y4m'), 'r+b') as f:
            f.seek(frame['offset'] + 10)
            f.write(b'\x00\xff')
        status, _, body = env.http('GET', f'/api/sessions/{sid}/frames/{frame["frame_id"]}.bmp', token='view')
        self.assertEqual(status, 409, body)


if __name__ == '__main__':
    unittest.main()
