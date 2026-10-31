import json
import threading
import unittest

from tracking_runtime import LiveTrackingRuntime


class Clock:
    now = 10.0

    def __call__(self):
        return self.now


class Tracker:
    def __init__(self):
        self.calls = 0
        self.hook = lambda: None

    def snapshot(self):
        return dict(box=None, accepted=False, confidence=0, state='SELECT TARGET',
                    reason='', trail=[], velocity={'x': 0, 'y': 0})

    def initialize(self, frame, roi, timestamp):
        return dict(self.snapshot(), ok=True, state='INITIALIZING')

    def update(self, frame, timestamp):
        self.calls += 1
        self.hook()
        return dict(self.snapshot(), accepted=True, confidence=0.9, state='TRACKING',
                    box={'x': 10, 'y': 10, 'w': 20, 'h': 20}, trail=[{'x': 20, 'y': 20}])


class RuntimeChecks(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.tracker = Tracker()
        self.runtime = LiveTrackingRuntime(0.2, tracker=self.tracker, clock=self.clock)
        self.runtime.initialize(None, {}, self.clock())
        self.clock.now += 0.01

    def accepted(self):
        result = self.runtime.update(None, self.clock())
        self.assertTrue(result['observation_valid'])
        return result

    def test_expiration_without_another_camera_frame(self):
        self.accepted()
        self.clock.now += 0.201
        result = self.runtime.snapshot()
        self.assertFalse(result['accepted'])
        self.assertFalse(result['observation_valid'])
        self.assertEqual(result['state'], 'LOST')
        self.assertEqual(result['confidence'], 0)
        self.assertIsNone(result['trail'][-1])
        self.assertFalse(self.runtime.update(None, self.clock())['accepted'])
        self.assertEqual(self.tracker.calls, 1)
        self.runtime.initialize(None, {}, self.clock())
        self.clock.now += 0.01
        self.accepted()

    def test_duplicate_and_older_frames_do_not_refresh_age(self):
        accepted = self.accepted()
        self.clock.now += 0.05
        for captured_at in (accepted['capture_timestamp_s'], accepted['capture_timestamp_s'] - 0.001):
            result = self.runtime.update(None, captured_at)
            self.assertFalse(result['submitted_frame_processed'])
            self.assertEqual(result['capture_timestamp_s'], accepted['capture_timestamp_s'])
        self.assertEqual(self.tracker.calls, 1)
        self.clock.now += 0.16
        self.assertFalse(self.runtime.snapshot()['observation_valid'])

    def test_delayed_inference_result_cannot_be_published(self):
        def slow():
            self.clock.now += 0.21
        self.tracker.hook = slow
        result = self.runtime.update(None, self.clock())
        self.assertFalse(result['accepted'])
        self.assertIn('DEADLINE', result['runtime_fault'])

    def test_camera_disconnect_invalidates_an_accepted_result(self):
        self.accepted()
        result = self.runtime.mark_camera_failure()
        self.assertFalse(result['observation_valid'])
        self.assertIn('CAMERA FAILURE', result['reason'])

    def test_tracker_exception_is_latched(self):
        self.accepted()
        self.clock.now += 0.01
        def broken():
            raise RuntimeError('inference failed')
        self.tracker.hook = broken
        with self.assertLogs('tracking_runtime', level='ERROR'):
            result = self.runtime.update(None, self.clock())
        self.assertFalse(result['accepted'])
        self.assertIn('TRACKER FAILURE', result['reason'])
        self.assertEqual(result['confidence'], 0)

    def test_invalid_capture_timestamps_are_not_sent_to_tracker(self):
        for timestamp in (float('nan'), float('inf'), 'invalid', None, 11, 9):
            with self.subTest(timestamp=timestamp):
                runtime = LiveTrackingRuntime(0.2, tracker=Tracker(), clock=self.clock)
                result = runtime.initialize(None, {}, timestamp)
                self.assertFalse(result['accepted'])
                self.assertIsNotNone(result['runtime_fault'])
                json.dumps(result, allow_nan=False)

    def test_snapshot_cannot_mutate_published_result(self):
        result = self.accepted()
        result['box']['x'] = -999
        result['trail'][0]['x'] = -999
        snapshot = self.runtime.snapshot()
        self.assertEqual(snapshot['box']['x'], 10)
        self.assertEqual(snapshot['trail'][0]['x'], 20)

    def test_watchdog_remains_available_during_blocked_inference(self):
        self.accepted()
        self.clock.now += 0.01
        entered, release = threading.Event(), threading.Event()
        def blocked():
            entered.set()
            if not release.wait(2):
                raise RuntimeError('test inference timed out')
        self.tracker.hook = blocked
        thread = threading.Thread(target=self.runtime.update, args=(None, self.clock()))
        thread.start()
        try:
            self.assertTrue(entered.wait(1))
            result = self.runtime.update(None, self.clock())
            self.assertTrue(result['busy'])
            self.assertEqual(self.tracker.calls, 2)
            self.clock.now += 0.201
            result = self.runtime.snapshot()
            self.assertTrue(result['processing'])
            self.assertFalse(result['observation_valid'])
        finally:
            release.set()
            thread.join(2)
        self.assertFalse(thread.is_alive())
        self.assertFalse(self.runtime.snapshot()['accepted'])

    def test_failed_selection_does_not_reuse_previous_target(self):
        self.accepted()
        self.tracker.initialize = lambda *args: {'ok': False, 'reason': 'bad ROI'}
        result = self.runtime.initialize(None, {}, self.clock())
        self.assertFalse(result['accepted'])
        self.assertIsNone(result['box'])
        self.assertIn('INITIALIZATION FAILED', result['reason'])

    def test_age_limit_must_be_explicit_and_positive(self):
        for limit in (0, -1, float('nan'), float('inf')):
            with self.subTest(limit=limit), self.assertRaises(ValueError):
                LiveTrackingRuntime(limit, tracker=self.tracker)

    def test_core_reselection_request_latches_runtime_fault(self):
        self.tracker.update = lambda *args: dict(self.tracker.snapshot(),
                                                manual_required=True, reason='MODEL FAILURE')
        result = self.runtime.update(None, self.clock())
        self.assertFalse(result['observation_valid'])
        self.assertEqual(result['runtime_fault'], 'MODEL FAILURE')

    def test_delayed_initialization_cannot_report_success(self):
        def slow(*args):
            self.clock.now += 0.21
            return dict(self.tracker.snapshot(), ok=True)
        self.tracker.initialize = slow
        result = self.runtime.initialize(None, {}, self.clock())
        self.assertFalse(result['ok'])
        self.assertFalse(result['observation_valid'])


if __name__ == '__main__':
    unittest.main()
