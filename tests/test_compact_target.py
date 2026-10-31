import unittest

import cv2
import numpy as np

from compact_target import CompactTarget
from precision_tracker import PrecisionTracker


def bird_scene(target=(80, 100), other=None):
    image = np.full((240, 400, 3), 185, np.uint8)
    for position in (target, other):
        if position is None:
            continue
        x, y = position
        # Flared wings and body, with only a small fraction of the ROI occupied.
        polygon = np.array([[x - 8, y - 3], [x - 1, y], [x, y - 5],
                            [x + 2, y], [x + 9, y - 4], [x + 4, y + 3],
                            [x + 1, y + 5], [x - 2, y + 2]], np.int32)
        cv2.fillPoly(image, [polygon], (20, 20, 20))
    return image


class CompactTargetChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cv2.setNumThreads(2)

    def tracker(self):
        tracker = PrecisionTracker()
        result = tracker.initialize(bird_scene(other=(300, 100)), dict(x=65, y=85, w=30, h=30), 0)
        self.assertTrue(result['ok'])
        self.assertIsNotNone(tracker.compact_target)
        return tracker

    def test_small_silhouette_tracks_motion_without_tracking_the_background(self):
        tracker = self.tracker()
        original = tracker.matcher.template_features.copy()
        for index in range(1, 16):
            x = 80 + index * 8
            result = tracker.update(bird_scene((x, 100), (300, 140)), index * 33)
            self.assertTrue(result['accepted'])
            self.assertEqual(result['measurement_source'], 'compact-contrast-motion')
            self.assertLess(abs(result['box']['x'] + result['box']['w'] / 2 - x), 3)
            self.assertLess(abs(result['box']['y'] + result['box']['h'] / 2 - 100), 3)
        np.testing.assert_array_equal(original, tracker.matcher.template_features)

    def test_disappearance_stops_tracking_and_breaks_the_path(self):
        tracker = self.tracker()
        accepted = tracker.update(bird_scene((88, 100), (300, 140)), 33)
        result = tracker.update(bird_scene(None, (300, 140)), 66)
        self.assertEqual(result['state'], 'LOST')
        self.assertFalse(result['accepted'])
        self.assertTrue(result['manual_required'])
        self.assertEqual(result['box'], accepted['box'])
        self.assertIsNone(result['trail'][-1])
        # Even if a new identical bird appears nearby, it cannot take over.
        for timestamp in (99, 132, 165):
            result = tracker.update(bird_scene((104, 100), (300, 140)), timestamp)
            self.assertFalse(result['accepted'])
            self.assertEqual(result['box'], accepted['box'])

    def test_crossing_identical_targets_require_reselection(self):
        tracker = self.tracker()
        rejected = None
        for index in range(1, 15):
            result = tracker.update(bird_scene((80 + index * 8, 100), (220 - index * 8, 100)), index * 33)
            if not result['accepted']:
                rejected = result
                break
        self.assertIsNotNone(rejected)
        self.assertTrue(rejected['manual_required'])
        self.assertTrue('AMBIGUOUS' in rejected['reason'] or 'MERGE' in rejected['reason'])
        self.assertIsNone(rejected['trail'][-1])
        self.assertFalse(tracker.update(bird_scene((200, 100), (100, 100)), 600)['accepted'])

    def test_frame_gap_cannot_reuse_a_small_target_identity(self):
        tracker = self.tracker()
        result = tracker.update(bird_scene((88, 100)), 300)
        self.assertFalse(result['accepted'])
        self.assertIn('FRAME GAP', result['reason'])

    def test_duplicate_frames_do_not_change_compact_motion_state(self):
        tracker = self.tracker()
        expected = tracker.update(bird_scene((88, 100)), 33)
        for timestamp in (33, 0):
            actual = tracker.update(bird_scene((200, 100)), timestamp)
            expected.pop('latency_ms', None)
            self.assertEqual(actual, expected)

    def test_selecting_multiple_silhouettes_returns_an_actionable_error(self):
        tracker = PrecisionTracker()
        result = tracker.initialize(bird_scene((80, 100), (110, 100)), dict(x=60, y=80, w=70, h=40), 0)
        self.assertFalse(result['ok'])
        self.assertIn('multiple', result['reason'])

    def test_new_selection_clears_a_compact_identity_loss(self):
        tracker = self.tracker()
        tracker.update(bird_scene(None), 33)
        result = tracker.initialize(bird_scene((200, 100)), dict(x=185, y=85, w=30, h=30), 66)
        self.assertTrue(result['ok'])
        self.assertFalse(result['manual_required'])
        self.assertEqual(result['trail'], [])
        self.assertTrue(tracker.update(bird_scene((208, 100)), 99)['accepted'])

    def test_large_or_textured_selections_keep_the_neural_tracker(self):
        image = np.random.default_rng(12).integers(0, 256, (240, 400, 3), dtype=np.uint8)
        self.assertIsNone(CompactTarget.from_selection(image, np.array([50, 50, 50, 60], np.float32)))
        self.assertIsNone(CompactTarget.from_selection(bird_scene(), np.array([10, 10, 130, 130], np.float32)))


if __name__ == '__main__':
    unittest.main()
