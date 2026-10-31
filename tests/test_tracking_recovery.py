import unittest

import cv2
import numpy as np

from precision_tracker import PrecisionTracker
from tools.benchmark_synthetic import sequence, overlap, texture


class RecoveryChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cv2.setNumThreads(2)
        cv2.setRNGSeed(1701)

    def run_sequence(self, name):
        frames = iter(sequence(name))
        frame, box = next(frames)
        tracker = PrecisionTracker()
        tracker.initialize(frame, dict(zip(('x', 'y', 'w', 'h'), box)), 0)
        original = tracker.matcher.template_features.copy()
        results = []
        for index, (frame, truth) in enumerate(frames, 1):
            result = tracker.update(frame, index * 1000 / 30)
            box = [result['box'][key] for key in ('x', 'y', 'w', 'h')]
            results.append((index, truth, result, overlap(box, truth) if truth else 0))
        np.testing.assert_array_equal(original, tracker.matcher.template_features)
        return tracker, results

    def test_recovers_target_that_moves_behind_full_occlusion(self):
        _, results = self.run_sequence('occlusion')
        self.assertTrue(all(not r['accepted'] for _, gt, r, _ in results if gt is None))
        reappeared = [(i, r, iou) for i, gt, r, iou in results if i >= 50]
        # Confirm across three frames, then regain the displaced object.
        self.assertFalse(reappeared[0][1]['accepted'])
        self.assertFalse(reappeared[1][1]['accepted'])
        self.assertTrue(all(r['accepted'] and iou >= 0.5 for _, r, iou in reappeared[2:]))

    def test_similar_color_distractor_is_rejected_during_occlusion(self):
        _, results = self.run_sequence('distractor')
        self.assertTrue(all(not r['accepted'] for _, gt, r, _ in results if gt is None))
        self.assertTrue(all(r['accepted'] and iou >= 0.5 for i, gt, r, iou in results if i >= 53))

    def test_identical_remote_candidates_cannot_pass_reference_search(self):
        tracker = PrecisionTracker()
        frame = np.full((360, 640, 3), (45, 100, 50), np.uint8)
        frame[120:210, 120:180] = texture()
        tracker.initialize(frame, dict(x=120, y=120, w=60, h=90), 0)
        frame[120:210, 450:510] = texture()
        self.assertIsNone(tracker.search_reference(frame, wide=True))
        self.assertIn('AMBIGUOUS', tracker.reason)

    def test_duplicate_frame_cannot_confirm_recovery(self):
        frames = list(sequence('occlusion'))
        tracker = PrecisionTracker()
        frame, box = frames[0]
        tracker.initialize(frame, dict(zip(('x', 'y', 'w', 'h'), box)), 0)
        for i in range(1, 51):
            tracker.update(frames[i][0], i * 1000 / 30)
        confirmations = tracker.recovery_count
        for _ in range(5):
            result = tracker.update(frames[50][0], 50 * 1000 / 30)
            self.assertFalse(result['accepted'])
            self.assertEqual(tracker.recovery_count, confirmations)


if __name__ == '__main__':
    unittest.main()
