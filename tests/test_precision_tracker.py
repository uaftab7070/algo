import unittest
from unittest.mock import patch
import cv2
import numpy as np
from precision_tracker import PrecisionTracker


def scene(shift=0, covered=False):
    image = np.full((240, 400, 3), (35, 100, 40), np.uint8)
    # A textured selected item with edges suitable for a forward/backward flow check.
    for y in range(65, 155):
        for x in range(140, 200):
            value = int(100 + 60 * np.sin(x * 0.3) + 40 * np.cos(y * 0.4))
            image[y, x + shift] = (max(0, value - 30), min(255, value + 20), 230)
    if covered:
        image[50:170, 125:220] = (35, 100, 40)
    return image


class PrecisionChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cv2.setNumThreads(2)

    def tracker(self):
        tracker = PrecisionTracker()
        result = tracker.initialize(scene(), dict(x=140, y=65, w=60, h=90), 0)
        self.assertTrue(result['ok'])
        return tracker

    def test_optical_flow_measures_known_translation(self):
        tracker = self.tracker()
        flow = tracker.flow(cv2.cvtColor(scene(shift=4), cv2.COLOR_BGR2GRAY))
        self.assertIsNotNone(flow)
        moved_x = flow['box'][0] + flow['box'][2] / 2
        self.assertLess(abs(moved_x - 174), 1.0)
        self.assertGreaterEqual(flow['count'], tracker.config.flow_min_points)

    def test_missing_target_cannot_remain_confirmed(self):
        tracker = self.tracker()
        original = tracker.matcher.template_features.copy()
        for i in range(12):
            result = tracker.update(scene(covered=True), 33 * (i + 1))
        self.assertEqual(result['state'], 'LOST')
        self.assertFalse(result['accepted'])
        self.assertLess(result['confidence'], 0.5)
        np.testing.assert_array_equal(original, tracker.matcher.template_features)

    def test_scene_cut_requires_manual_reinitialization(self):
        tracker = self.tracker()
        self.assertTrue(tracker.update(scene(), 16)['accepted'])
        cut = np.full((240, 400, 3), (200, 20, 10), np.uint8)
        result = tracker.update(cut, 33)
        self.assertEqual(result['state'], 'LOST')
        self.assertIn('SCENE CUT', result['reason'])
        self.assertEqual(result['confidence'], 0)
        self.assertEqual(result['measurement_source'], 'none')
        self.assertEqual(result['flow_inliers'], 0)
        self.assertIsNone(result['trail'][-1])
        reason = result['reason']
        for i in range(5):
            result = tracker.update(scene(), 66 + i * 33)
            self.assertEqual(result['reason'], reason)
        self.assertFalse(result['accepted'])
        self.assertTrue(tracker.manual_required)

    def test_source_size_change_clears_current_visual_evidence(self):
        tracker = self.tracker()
        result = tracker.update(scene(shift=4), 33)
        self.assertTrue(result['accepted'])
        result = tracker.update(cv2.resize(scene(), (800, 480)), 66)
        self.assertEqual(result['state'], 'LOST')
        self.assertFalse(result['accepted'])
        self.assertEqual(result['confidence'], 0)
        self.assertEqual(result['velocity'], {'x': 0, 'y': 0})
        self.assertEqual(result['flow_inliers'], 0)
        self.assertEqual(result['measurement_source'], 'none')
        self.assertIsNone(result['trail'][-1])
        self.assertIsNone(tracker.previous_gray)

    def test_source_change_stays_lost_until_new_selection(self):
        tracker = self.tracker()
        changed = cv2.resize(scene(), (800, 480))
        result = tracker.update(changed, 33)
        reason = result['reason']
        for timestamp in (66, 99):
            result = tracker.update(scene(), timestamp)
            self.assertEqual(result['state'], 'LOST')
            self.assertEqual(result['reason'], reason)
            self.assertFalse(result['accepted'])
        result = tracker.initialize(changed, dict(x=280, y=130, w=120, h=180), 100)
        self.assertTrue(result['ok'])
        self.assertFalse(tracker.manual_required)
        self.assertEqual(result['state'], 'INITIALIZING')
        self.assertEqual(result['trail'], [])

    def test_stale_frame_with_different_size_cannot_invalidate_tracking(self):
        tracker = self.tracker()
        expected = tracker.snapshot()
        for timestamp in (0, -33):
            result = tracker.update(cv2.resize(scene(), (800, 480)), timestamp)
            self.assertEqual(result, expected)
            self.assertFalse(tracker.manual_required)

    def test_invalid_timestamp_cannot_invalidate_tracking(self):
        tracker = self.tracker()
        expected = tracker.snapshot()
        for timestamp in (float('nan'), float('inf'), float('-inf')):
            with self.subTest(timestamp=timestamp), self.assertRaises(ValueError):
                tracker.update(cv2.resize(scene(), (800, 480)), timestamp)
            self.assertEqual(tracker.snapshot(), expected)

    def test_invalid_frames_are_rejected_before_state_changes(self):
        tracker = self.tracker()
        expected = tracker.snapshot()
        for frame in (None, np.zeros((0, 10, 3), np.uint8),
                      np.zeros((10, 10), np.uint8), np.zeros((10, 10, 4), np.uint8),
                      np.zeros((10, 10, 3), np.float32)):
            with self.subTest(shape=getattr(frame, 'shape', None)):
                with self.assertRaises(ValueError):
                    tracker.update(frame, 33)
                self.assertFalse(tracker.initialize(frame, dict(x=0, y=0, w=20, h=20))['ok'])
                self.assertEqual(tracker.snapshot(), expected)

    def test_malformed_selection_does_not_destroy_existing_target(self):
        tracker = self.tracker()
        expected = tracker.snapshot()
        for roi in (None, {}, dict(x='bad', y=0, w=20, h=20),
                    dict(x=0, y=0, w=[20], h=20), dict(x=0, y=0, w=float('inf'), h=20)):
            with self.subTest(roi=roi):
                self.assertFalse(tracker.initialize(scene(), roi)['ok'])
                self.assertEqual(tracker.snapshot(), expected)

    def test_invalid_model_output_clears_acceptance_and_remains_json_serializable(self):
        import json
        for box, score, psr in (([140, 65, float('nan'), 90], .9, 5),
                                ([140, 65, 0, 90], .9, 5), ([140, 65, 60, 90], float('nan'), 5),
                                ([140, 65, 60, 90], .9, float('inf'))):
            with self.subTest(box=box, score=score, psr=psr):
                tracker = self.tracker()
                self.assertTrue(tracker.update(scene(), 16)['accepted'])
                with patch.object(tracker.matcher, 'infer', return_value=(box, score, psr, False)):
                    result = tracker.update(scene(), 33)
                self.assertFalse(result['accepted'])
                self.assertEqual(result['state'], 'LOST')
                self.assertIn('MODEL FAILURE', result['reason'])
                json.dumps(result, allow_nan=False)


if __name__ == '__main__':
    unittest.main()
