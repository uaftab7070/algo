#!/usr/bin/env python3
"""Evaluate a manually selected target without skipping decoded video frames."""
import argparse
from collections import Counter
import csv
from dataclasses import asdict
import json
import math
from pathlib import Path
import sys
import time

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from precision_tracker import PrecisionTracker


def overlap(a, b):
    x0, y0 = max(a[0], b[0]), max(a[1], b[1])
    x1, y1 = min(a[0] + a[2], b[0] + b[2]), min(a[1] + a[3], b[1] + b[3])
    intersect = max(0, x1 - x0) * max(0, y1 - y0)
    return intersect / max(1e-12, a[2] * a[3] + b[2] * b[3] - intersect)


def load_ground_truth(path):
    """Read zero-based source-frame labels; visible=0 explicitly marks absence."""
    truth = {}
    with open(path, newline='') as source:
        reader = csv.DictReader(source)
        required = {'frame', 'x', 'y', 'width', 'height'}
        if not required.issubset(reader.fieldnames or []):
            raise ValueError('Ground truth requires frame,x,y,width,height columns (optional: visible).')
        for line, row in enumerate(reader, start=2):
            try:
                frame_id = int(row['frame'])
                if frame_id < 0 or frame_id in truth:
                    raise ValueError('frame numbers must be nonnegative and unique')
                visible = row.get('visible', '1').strip().lower()
                if visible not in ('1', '0', 'true', 'false'):
                    raise ValueError('visible must be 1, 0, true, or false')
                box = None
                if visible in ('1', 'true'):
                    box = [float(row[key]) for key in ('x', 'y', 'width', 'height')]
                    if not all(math.isfinite(value) for value in box) or min(box[2:]) <= 0:
                        raise ValueError('visible boxes must be finite and have positive width and height')
                truth[frame_id] = box
            except (ValueError, TypeError, AttributeError) as error:
                raise ValueError(f'Invalid ground truth at line {line}: {error}') from error
    return truth


def measure_frame(frame_id, result, truth):
    """Do not count a stale, rejected box as a successful localization."""
    accepted = bool(result.get('accepted', False))
    box = result.get('box')
    try:
        box = [float(box[key]) for key in ('x', 'y', 'w', 'h')]
        if not all(math.isfinite(value) for value in box) or min(box[2:]) <= 0:
            box = None
    except (KeyError, TypeError, ValueError):
        box = None
    labeled = frame_id in truth
    gt = truth.get(frame_id)
    visible = gt is not None if labeled else None
    iou, error = None, None
    if visible:
        iou = overlap(box, gt) if accepted and box is not None else 0.0
        if accepted and box is not None:
            error = math.hypot(box[0] + box[2] / 2 - gt[0] - gt[2] / 2,
                               box[1] + box[3] / 2 - gt[1] - gt[3] / 2)
    return {'frame': frame_id, 'labeled': labeled, 'target_visible': visible,
            'accepted': accepted, 'missing_prediction': box is None,
            'iou': iou, 'center_error_px': error}


def summarize_metrics(rows, annotation_count=None):
    """Rates are fractions in [0, 1]; unavailable denominators produce null."""
    labeled = [row for row in rows if row['labeled']]
    visible = [row for row in labeled if row['target_visible']]
    absent = [row for row in labeled if not row['target_visible']]
    accepted = sum(row['accepted'] for row in rows)
    labeled_accepted = sum(row['accepted'] for row in labeled)
    visible_accepted = sum(row['accepted'] for row in visible)
    absent_accepted = sum(row['accepted'] for row in absent)
    false_locks = absent_accepted + sum(row['accepted'] and row['iou'] < 0.1 for row in visible)
    ratio = lambda count, total: count / total if total else None
    summary = {
        'status': 'measured_on_labeled_frames' if labeled else 'unmeasured',
        'processed_frames': len(rows), 'annotated_frames': len(labeled),
        'unlabeled_frames': len(rows) - len(labeled),
        'annotation_rows_outside_processed_frames': annotation_count - len(labeled) if annotation_count is not None else None,
        'visible_frames': len(visible), 'absent_frames': len(absent),
        'accepted_frames': accepted, 'rejected_frames': len(rows) - accepted,
        'missing_prediction_frames': sum(row['missing_prediction'] for row in rows),
        'annotated_accepted_frames': labeled_accepted,
        'annotated_rejected_frames': len(labeled) - labeled_accepted,
        'accepted_visible_frames': visible_accepted,
        'rejected_visible_frames': len(visible) - visible_accepted,
        'accepted_absent_frames': absent_accepted,
        'missing_visible_predictions': sum(row['missing_prediction'] for row in visible),
        'annotation_coverage': ratio(len(labeled), len(rows)),
        'accepted_frame_fraction': ratio(accepted, len(rows)),
        'visible_tracking_coverage': ratio(visible_accepted, len(visible)),
        'false_lock_frames': false_locks if labeled else None,
        'false_lock_fraction_of_accepted': ratio(false_locks, labeled_accepted),
        'mean_iou': sum(row['iou'] for row in visible) / len(visible) if visible else None,
    }
    for threshold, suffix in ((0.5, '0_5'), (0.75, '0_75')):
        successes = sum(row['accepted'] and row['iou'] >= threshold for row in visible)
        summary[f'success_frames_iou_{suffix}'] = successes if visible else None
        summary[f'success_rate_iou_{suffix}'] = ratio(successes, len(visible))
        summary[f'precision_iou_{suffix}'] = ratio(successes, labeled_accepted)
        summary[f'incorrect_accepted_frames_iou_{suffix}'] = labeled_accepted - successes if labeled else None
        summary[f'decision_accuracy_iou_{suffix}'] = ratio(successes + len(absent) - absent_accepted, len(labeled))
    errors = [row['center_error_px'] for row in visible if row['center_error_px'] is not None]
    summary['mean_accepted_center_error_px'] = sum(errors) / len(errors) if errors else None
    summary['definitions'] = {
        'success_rate': 'Accepted predictions with IoU >= threshold / all labeled visible frames; rejected or missing predictions fail.',
        'precision': 'Accepted predictions with IoU >= threshold / all accepted labeled frames; false acceptance during absence fails.',
        'decision_accuracy': '(Successful visible predictions + correct rejection on explicitly absent frames) / all labeled frames.',
        'false_lock': 'Accepted prediction on an absent frame, or accepted visible prediction with IoU < 0.1.',
        'coverage': 'Annotation coverage is labeled/processed; visible tracking coverage is accepted/labeled-visible. Neither is accuracy.',
        'scope': 'Initialization is excluded. Missing labels are unmeasured, not absent. Rates describe only evaluated annotations and do not establish 99% accuracy on other videos.',
    }
    return summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('video')
    parser.add_argument('--roi', required=True, help='x,y,width,height in the source image')
    parser.add_argument('--start-frame', type=int, default=0)
    parser.add_argument('--frames', type=int, default=240)
    parser.add_argument('--ground-truth', help='Optional zero-based CSV: frame,x,y,width,height[,visible]. Use visible=0 for absent targets.')
    parser.add_argument('--output', default='artifacts/precision-benchmark')
    args = parser.parse_args()
    if args.start_frame < 0 or args.frames < 1:
        parser.error('--start-frame must be nonnegative and --frames must be positive')
    try:
        values = list(map(float, args.roi.split(',')))
        if len(values) != 4 or not all(math.isfinite(value) for value in values) or min(values[2:]) <= 0:
            raise ValueError
    except ValueError:
        parser.error('--roi requires four finite numbers with positive width and height')
    roi = dict(zip(('x', 'y', 'w', 'h'), values))
    try:
        truth = load_ground_truth(args.ground_truth) if args.ground_truth else {}
    except (OSError, ValueError) as error:
        parser.error(str(error))
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    cv2.setNumThreads(2)
    cap = cv2.VideoCapture(args.video)
    cap.set(cv2.CAP_PROP_POS_FRAMES, args.start_frame)
    fps = cap.get(cv2.CAP_PROP_FPS)
    fps = fps if math.isfinite(fps) and fps > 0 else 30
    ok, frame = cap.read()
    if not ok:
        raise SystemExit('Could not read the initialization frame')
    tracker = PrecisionTracker()
    initialized = tracker.initialize(frame, roi, args.start_frame / fps * 1000)
    if not initialized['ok']:
        raise SystemExit(initialized['reason'])
    counts, latencies, shots, metrics = Counter(), [], [], []
    sample_frames = set(int(x) for x in np.linspace(1, args.frames, min(9, args.frames)))
    with (out / 'frames.jsonl').open('w') as log:
        for offset in range(1, args.frames + 1):
            ok, frame = cap.read()
            if not ok:
                break
            frame_id = args.start_frame + offset
            start = time.perf_counter()
            result = tracker.update(frame, frame_id / fps * 1000)
            latencies.append((time.perf_counter() - start) * 1000)
            counts[result['state']] += 1
            box = [result['box'][key] for key in ('x', 'y', 'w', 'h')] if result.get('box') else None
            log.write(json.dumps({'frame': frame_id, **result}) + '\n')
            metrics.append(measure_frame(frame_id, result, truth))
            if offset in sample_frames:
                color = (50, 255, 80) if result['accepted'] else (60, 60, 255)
                if not metrics[-1]['missing_prediction']:
                    x, y, w, h = map(int, box)
                    cv2.rectangle(frame, (x, y), (x + w, y + h), color, 2)
                cv2.rectangle(frame, (210, 32), (900, 78), (10, 20, 10), -1)
                cv2.putText(frame, f"Frame {frame_id}  {result['state']}  conf {result['confidence']:.2f}", (220, 65), cv2.FONT_HERSHEY_SIMPLEX, 0.7, color, 2)
                shots.append(cv2.resize(frame, (480, round(frame.shape[0] * 480 / frame.shape[1]))))
    cap.release()
    accuracy = summarize_metrics(metrics, len(truth) if args.ground_truth else None)
    summary = {'video': str(Path(args.video).resolve()), 'start_frame': args.start_frame,
               'initial_roi': roi, 'frames': len(latencies), 'states': dict(counts),
               'mean_core_ms': float(np.mean(latencies)) if latencies else None,
               'p95_core_ms': float(np.percentile(latencies, 95)) if latencies else None,
               'config': asdict(tracker.config), 'ground_truth_frames': accuracy['annotated_frames'],
               'accuracy': accuracy,
               'note': 'Tracking states are not an accuracy score. Timing excludes browser encoding, network, decoding and display.'}
    if accuracy['annotated_frames']:
        summary['mean_iou'] = accuracy['mean_iou']
        summary['mean_accepted_center_error_px'] = accuracy['mean_accepted_center_error_px']
        summary['ground_truth_metrics'] = [row for row in metrics if row['labeled']]
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    if shots:
        while len(shots) % 3:
            shots.append(np.zeros_like(shots[0]))
        cv2.imwrite(str(out / 'contact.jpg'), np.vstack([np.hstack(shots[i:i + 3]) for i in range(0, len(shots), 3)]))
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
