import tempfile
from pathlib import Path
import unittest

from tools.benchmark_precision import load_ground_truth, measure_frame, overlap, summarize_metrics


def prediction(box=(0, 0, 10, 10), accepted=True):
    return {'box': dict(zip(('x', 'y', 'w', 'h'), box)) if box is not None else None,
            'accepted': accepted}


class AccuracyMetricsChecks(unittest.TestCase):
    def test_rejections_and_false_locks_count_against_labeled_accuracy(self):
        truth = {frame: [0, 0, 10, 10] for frame in range(1, 5)}
        truth.update({5: None, 6: None})
        results = [prediction(), prediction((0, 0, 8, 8)), prediction(accepted=False),
                   prediction((50, 50, 10, 10)), prediction(), prediction(accepted=False), prediction()]
        rows = [measure_frame(frame, result, truth) for frame, result in enumerate(results, 1)]
        metrics = summarize_metrics(rows, annotation_count=9)
        self.assertEqual(metrics['processed_frames'], 7)
        self.assertEqual(metrics['annotated_frames'], 6)
        self.assertEqual(metrics['unlabeled_frames'], 1)
        self.assertEqual(metrics['annotation_rows_outside_processed_frames'], 3)
        self.assertEqual(metrics['accepted_frames'], 5)
        self.assertEqual(metrics['rejected_frames'], 2)
        self.assertEqual(metrics['visible_tracking_coverage'], 3 / 4)
        self.assertEqual(metrics['success_rate_iou_0_5'], 2 / 4)
        self.assertEqual(metrics['success_rate_iou_0_75'], 1 / 4)
        self.assertEqual(metrics['precision_iou_0_5'], 2 / 4)
        self.assertEqual(metrics['decision_accuracy_iou_0_5'], 3 / 6)
        self.assertEqual(metrics['false_lock_frames'], 2)
        self.assertEqual(metrics['false_lock_fraction_of_accepted'], 2 / 4)
        self.assertEqual(metrics['incorrect_accepted_frames_iou_0_5'], 2)
        self.assertEqual(rows[2]['iou'], 0)
        self.assertIsNone(rows[2]['center_error_px'])

    def test_unlabeled_frames_cannot_produce_an_accuracy_claim(self):
        metrics = summarize_metrics([measure_frame(1, prediction(), {})])
        self.assertEqual(metrics['status'], 'unmeasured')
        self.assertEqual(metrics['accepted_frame_fraction'], 1)
        self.assertEqual(metrics['annotation_coverage'], 0)
        self.assertIsNone(metrics['success_rate_iou_0_5'])
        self.assertIsNone(metrics['mean_iou'])
        self.assertIsNone(metrics['false_lock_frames'])

    def test_empty_run_is_unmeasured_without_nan_or_division_by_zero(self):
        metrics = summarize_metrics([], annotation_count=2)
        self.assertEqual(metrics['processed_frames'], 0)
        self.assertEqual(metrics['annotation_rows_outside_processed_frames'], 2)
        self.assertIsNone(metrics['annotation_coverage'])
        self.assertIsNone(metrics['accepted_frame_fraction'])
        self.assertIsNone(metrics['success_rate_iou_0_75'])

    def test_missing_and_invalid_accepted_boxes_are_failures(self):
        truth = {1: [0, 0, 10, 10], 2: [0, 0, 10, 10]}
        rows = [measure_frame(1, prediction(None), truth),
                measure_frame(2, prediction((float('nan'), 0, 10, 10)), truth)]
        metrics = summarize_metrics(rows, 2)
        self.assertEqual(metrics['missing_prediction_frames'], 2)
        self.assertEqual(metrics['missing_visible_predictions'], 2)
        self.assertEqual(metrics['success_rate_iou_0_5'], 0)
        self.assertEqual(metrics['false_lock_frames'], 2)

    def test_absence_rejection_does_not_inflate_visible_tracking_success(self):
        rows = [measure_frame(1, prediction(accepted=False), {1: None})]
        metrics = summarize_metrics(rows, 1)
        self.assertIsNone(metrics['success_rate_iou_0_5'])
        self.assertIsNone(metrics['visible_tracking_coverage'])
        self.assertEqual(metrics['decision_accuracy_iou_0_5'], 1)
        self.assertEqual(metrics['false_lock_frames'], 0)

    def test_iou_handles_subpixel_boxes(self):
        self.assertAlmostEqual(overlap([0, 0, .1, .1], [0, 0, .1, .1]), 1)

    def read_csv(self, content):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'labels.csv'
            path.write_text(content)
            return load_ground_truth(path)

    def test_csv_supports_existing_boxes_and_explicit_absence(self):
        self.assertEqual(self.read_csv('frame,x,y,width,height\n0,1,2,3,4\n'), {0: [1, 2, 3, 4]})
        self.assertEqual(self.read_csv('frame,x,y,width,height,visible\n1,,,,,0\n'), {1: None})

    def test_invalid_labels_fail_instead_of_silently_changing_denominators(self):
        for csv_text in ('frame,x,y\n0,1,2\n',
                         'frame,x,y,width,height\n0,1,2,3,4\n0,1,2,3,4\n',
                         'frame,x,y,width,height\n0,nan,2,3,4\n',
                         'frame,x,y,width,height\n0,1,2,0,4\n',
                         'frame,x,y,width,height\n-1,1,2,3,4\n',
                         'frame,x,y,width,height,visible\n0,1,2,3,4,maybe\n'):
            with self.subTest(csv=csv_text), self.assertRaises(ValueError):
                self.read_csv(csv_text)


if __name__ == '__main__':
    unittest.main()
