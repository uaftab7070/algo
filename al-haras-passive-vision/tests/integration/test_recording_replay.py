"""Recording, replay, reporting and annotation integration tests (host build)."""
import csv
import json
import os
import sys
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from pyharness import NOTICE, Env, verify_independently  # noqa: E402


class RecordingTests(unittest.TestCase):
    def test_ac004_ac005_record_verify_and_independent_check(self):
        env = Env('ac004')
        run = env.record()
        self.assertEqual(run.code, 0, run.stderr)
        [sid] = run.json['sessions']
        verify = env.cli('verify', sid, token=False)
        self.assertEqual(verify.code, 0, verify.stdout)
        self.assertTrue(verify.json['ok'])
        self.assertEqual(verify.json['summary']['frames_stored'], 90)
        code, independent = verify_independently('session', env.session_dir(sid))
        self.assertEqual(code, 0, independent)
        manifest = env.manifest(sid)
        self.assertEqual(manifest['usage_notice'], NOTICE)
        self.assertEqual(manifest['status'], 'complete')
        frames = env.lines(sid, 'frames.jsonl')
        self.assertEqual([f['frame_id'] for f in frames], list(range(90)))
        self.assertTrue(all(a['acquisition_mono_ns'] < b['acquisition_mono_ns'] for a, b in zip(frames, frames[1:])))
        # AC-023: both clock domains are recorded and documented.
        self.assertEqual(manifest['clocks']['ordering_clock'], 'CLOCK_MONOTONIC')
        self.assertEqual(manifest['source']['timestamp_basis'], 'file_nominal_frame_rate')

    def test_ac006_effective_settings_come_from_the_source(self):
        env = Env('ac006', source='static.y4m')
        run = env.record()
        [sid] = run.json['sessions']
        source = env.manifest(sid)['source']
        # The configuration names only a file; geometry and rate are what the input actually declares.
        self.assertEqual((source['width'], source['height'], source['pixel_format']), (160, 120, 'i420'))
        self.assertEqual(source['effective']['nominal_fps'], [30, 1])
        self.assertEqual(source['content_origin'], 'synthetic_fixture')
        self.assertEqual(env.manifest(sid)['configuration']['source']['path'], env.source)

    def test_ac007_replay_reproducibility(self):
        reports = []
        for label in ('ac007a', 'ac007b'):
            env = Env(label, source='illumination.y4m')
            run = env.record()
            [sid] = run.json['sessions']
            bundle = env.cli('report', sid, token=False).json
            with open(os.path.join(bundle['report_dir'], 'qa-report.json')) as f:
                reports.append(json.load(f))
        self.assertEqual(reports[0]['deterministic'], reports[1]['deterministic'])
        self.assertEqual(reports[0]['environment']['video_sha256'], reports[1]['environment']['video_sha256'])
        # Documented non-deterministic fields: IDs, wall/monotonic times, processing timings.
        self.assertNotEqual(reports[0]['session_id'], reports[1]['session_id'])

    def test_ac009_ac012_report_bundle_conforms_and_is_traceable(self):
        env = Env('ac012', source='illumination.y4m')
        [sid] = env.record().json['sessions']
        bundle = env.cli('report', sid, '--contact-sheet', '6', token=False).json
        code, result = verify_independently('report', bundle['report_dir'])
        self.assertEqual(code, 0, result)
        with open(os.path.join(bundle['report_dir'], 'qa-report.json')) as f:
            report = json.load(f)
        env_section = report['environment']
        for key in ('recording_platform', 'recording_software', 'configuration_sha256', 'input_manifest_sha256', 'video_sha256', 'source'):
            self.assertIsNotNone(env_section[key], key)
        self.assertRegex(env_section['recording_software']['executable_sha256'], '^[0-9a-f]{64}$')
        # FR-022: the illumination ramp produces flagged dark and bright spans.
        reasons = {r for seg in report['deterministic']['unusable_segments'] for r in seg['reasons']}
        self.assertTrue({'very_dark', 'very_bright'} <= reasons, reasons)
        with open(os.path.join(bundle['report_dir'], 'summary.csv'), newline='') as f:
            rows = dict(csv.reader(f))
        self.assertEqual(rows['usage_notice'], NOTICE)
        self.assertTrue(os.path.getsize(os.path.join(bundle['report_dir'], 'contact-sheet.bmp')) > 54)

    def test_fr010_still_export_and_unknown_frame(self):
        env = Env('fr010', source='static.y4m')
        [sid] = env.record().json['sessions']
        out = os.path.join(env.root, 'still.bmp')
        still = env.cli('still', sid, '7', '--out', out, token=False)
        self.assertEqual(still.code, 0, still.stderr)
        with open(out, 'rb') as f:
            self.assertEqual(f.read(2), b'BM')
        self.assertIn('nearest-neighbour', still.json['transform'])
        missing = env.cli('still', sid, '999', '--out', out, token=False)
        self.assertNotEqual(missing.code, 0)
        self.assertIn('not stored', missing.stderr)

    def test_ac024_concurrent_sessions_have_unique_ids(self):
        env = Env('ac024', source='static.y4m')
        results = []
        threads = [threading.Thread(target=lambda: results.append(env.record())) for _ in range(4)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.assertTrue(all(r.code == 0 for r in results), [r.stderr for r in results])
        ids = [sid for r in results for sid in r.json['sessions']]
        self.assertEqual(len(set(ids)), 4)
        run_ids = {env.manifest(sid)['run_id'] for sid in ids}
        self.assertEqual(len(run_ids), 4)
        for sid in ids:
            self.assertTrue(env.cli('verify', sid, token=False).json['ok'])

    def test_fr018_segment_rollover_links_sessions(self):
        env = Env('fr018', source='static.y4m', extra={'recording.segment_max_frames': '25'})
        run = env.record()
        ids = run.json['sessions']
        self.assertEqual(len(ids), 3)  # 60 frames: 25 + 25 + 10
        manifests = [env.manifest(s) for s in ids]
        self.assertEqual([m['segment_index'] for m in manifests], [0, 1, 2])
        self.assertEqual([m['status'] for m in manifests], ['segment_complete', 'segment_complete', 'complete'])
        self.assertEqual(manifests[1]['previous_session_id'], ids[0])
        self.assertEqual(len({m['run_id'] for m in manifests}), 1)
        first = [f['frame_id'] for f in env.lines(ids[1], 'frames.jsonl')][0]
        self.assertEqual(first, 25)
        for sid in ids:
            self.assertTrue(env.cli('verify', sid, token=False).json['ok'])

    def test_nfr013_structured_log_schema(self):
        env = Env('nfr013', extra={'logging.level': 'info'})
        [sid] = env.record('--fault-injection', 'corrupt=5').json['sessions']
        with open(os.path.join(env.root, 'ahpv.log')) as f:
            records = [json.loads(line) for line in f]
        self.assertTrue(records)
        for r in records:
            self.assertRegex(r['event_id'], r'^[A-Z]{3}-[0-9]{3}$')
            self.assertEqual(r['software_version'], env.cli('version', token=False, config=False).json['version'])
            self.assertIn(r['level'], ('debug', 'info', 'warn', 'error'))
            self.assertIn('session_id', r)
        during = [r for r in records if r['event_id'] == 'CAP-020']
        self.assertEqual([r['session_id'] for r in during], [sid])
        self.assertIn('APP-001', {r['event_id'] for r in records})

    def test_recording_disabled_creates_no_session(self):
        env = Env('norec', extra={'recording.enabled': 'false'})
        run = env.record()
        self.assertEqual(run.code, 0, run.stderr)
        self.assertEqual(run.json['sessions'], [])


class AnnotationCliTests(unittest.TestCase):
    def test_ac008_ac022_annotation_governance(self):
        env = Env('ac008', source='static.y4m')
        [sid] = env.record().json['sessions']
        add = env.cli('annotate', 'add', sid, '--frame', '4', '--region', '10,10,30,20', '--notes', 'bench object A', token='ann')
        self.assertEqual(add.code, 0, add.stderr + add.stdout)
        aid = add.json['annotation_id']
        bad = env.cli('annotate', 'add', sid, '--frame', '4', '--region', '150,10,30,20', token='ann')
        self.assertEqual(bad.code, 1)
        self.assertIn('valid bounds', bad.json['message'])
        self.assertEqual(env.cli('annotate', 'add', sid, '--frame', '4', '--region', '1,1,2,2', token='view').code, 6)
        revise = env.cli('annotate', 'revise', sid, '--id', aid, '--expected-revision', '1', '--region', '12,10,30,20', token='ann2')
        self.assertEqual(revise.json['revision'], 2)
        stale = env.cli('annotate', 'revise', sid, '--id', aid, '--expected-revision', '1', '--notes', 'x', token='ann')
        self.assertEqual(stale.json['error'], 'revision_conflict')
        review = env.cli('annotate', 'review', sid, '--id', aid, '--expected-revision', '2', '--status', 'accepted', '--comment', 'ok', token='rev')
        self.assertEqual(review.json['reviewer_id'], 'rev')
        history = env.cli('annotate', 'history', sid, '--id', aid, token=False).json
        self.assertEqual([e['event'] for e in history['events']], ['create', 'revise', 'review'])
        self.assertEqual(history['events'][0]['annotation']['region_xywh_pixels'], [10, 10, 30, 20])  # Original preserved.
        current = env.cli('annotate', 'list', sid, token=False).json['annotations'][0]
        self.assertEqual((current['annotator_id'], current['revision'], current['coordinate_space']),
                         ('ann', 3, 'source_pixels_origin_top_left'))
        code, result = verify_independently('session', env.session_dir(sid))
        self.assertEqual(code, 0, result)
        actions = [(a['principal'], a['action'], a['outcome']) for a in env.audit()]
        self.assertIn(('view', 'annotation.add', 'denied'), actions)
        self.assertIn(('rev', 'annotation.review', 'allowed'), actions)
        report = env.cli('report', sid, token=False).json
        with open(os.path.join(report['report_dir'], 'qa-report.json')) as f:
            ann = json.load(f)['deterministic']['annotations']
        self.assertEqual((ann['active_annotations'], ann['independently_reviewed']), (1, 1))


if __name__ == '__main__':
    unittest.main()
