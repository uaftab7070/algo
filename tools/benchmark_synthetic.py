#!/usr/bin/env python3
"""Deterministic tracking stress cases with exact generated ground truth.

Uses the real NanoTrack ONNX models. This is a small diagnostic benchmark,
not evidence of accuracy on camera footage or a held-out real-world dataset.
Initialization is excluded from all metrics; rejected visible frames count as
failures. Hidden frames are scored separately and never given a box target.
"""
import argparse
from collections import Counter
import importlib.util
import json
from pathlib import Path
import sys
import time

import cv2
import numpy as np


ROOT = Path(__file__).resolve().parents[1]
CASES = ('stationary', 'translation', 'fast_motion', 'scale', 'occlusion', 'distractor')
FRAME_COUNTS = dict(stationary=36, translation=60, fast_motion=60, scale=60,
                    occlusion=72, distractor=72)


def overlap(a, b):
    left, top = max(a[0], b[0]), max(a[1], b[1])
    right = min(a[0] + a[2], b[0] + b[2])
    bottom = min(a[1] + a[3], b[1] + b[3])
    intersection = max(0, right - left) * max(0, bottom - top)
    return float(intersection / max(1, a[2] * a[3] + b[2] * b[3] - intersection))


def texture():
    """Generic textured red item; fixed seed and no downloaded image dependency."""
    rng = np.random.default_rng(20261023)
    yy, xx = np.mgrid[:90, :60]
    pattern = 100 + 50 * np.sin(xx * 0.3) + 35 * np.cos(yy * 0.4)
    pattern += rng.normal(0, 12, pattern.shape)
    item = np.stack((pattern - 30, pattern + 20, 220 + pattern * 0.1), axis=-1)
    item = np.clip(item, 0, 255).astype(np.uint8)
    cv2.rectangle(item, (3, 3), (56, 86), (25, 40, 165), 2)
    cv2.circle(item, (19, 25), 8, (175, 190, 240), 2)
    cv2.line(item, (14, 64), (44, 75), (30, 55, 150), 3)
    return item


def sequence(name):
    """Yield (frame, ground-truth xywh or None), including initialization."""
    if name not in CASES:
        raise ValueError(f'Unknown case: {name}')
    rng = np.random.default_rng(1701)
    background = np.empty((360, 640, 3), np.uint8)
    background[:] = (45, 100, 50)
    background = np.clip(background.astype(float) + rng.normal(0, 3, background.shape), 0, 255).astype(np.uint8)
    for y in range(30, 360, 50):
        cv2.line(background, (0, y), (639, y), (50, 106, 54), 1)
    item = texture()
    distractor = np.flip(item, axis=(0, 1)).copy()

    def paste(image, patch, box):
        x, y, w, h = map(int, box)
        image[y:y + h, x:x + w] = cv2.resize(patch, (w, h), interpolation=cv2.INTER_LINEAR)

    for index in range(FRAME_COUNTS[name] + 1):
        frame = background.copy()
        box = [120, 120, 60, 90]
        visible = True
        if name == 'translation':
            box[:2] = [120 + index * 4, 120 + round(16 * np.sin(index / 12))]
        elif name == 'fast_motion':
            distance = index * 40
            box[:2] = [70 + (distance % 800 if distance % 800 <= 400 else 800 - distance % 800),
                       115 + round(30 * np.sin(index / 8))]
        elif name == 'scale':
            factor = 1 + 0.65 * np.sin(np.pi * index / FRAME_COUNTS[name])
            w, h = round(60 * factor), round(90 * factor)
            box = [round(150 + index * 2 - w / 2), round(165 - h / 2), w, h]
        elif name == 'occlusion':
            visible = not 20 <= index < 50
            box[0] = 70 + index * 6
        elif name == 'distractor':
            box[0] = 100 + index * 4
            paste(frame, distractor, [490 - index * 4, 120, 60, 90])
            # Same-color alternative stays visible while the selected item is hidden.
            visible = not 39 <= index < 51
        if visible:
            paste(frame, item, box)
        yield frame, box if visible else None


def load_tracker(module_path):
    path = Path(module_path).resolve()
    spec = importlib.util.spec_from_file_location('benchmark_tracker_under_test', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    # A saved baseline lives in artifacts/, while both versions use the same models.
    module.MODEL_DIR = ROOT / 'models' / 'nanotrack'
    return module.PrecisionTracker


def evaluate_case(name, tracker_type, output=None):
    tracker = tracker_type()
    frames = iter(sequence(name))
    frame, initial_box = next(frames)
    initialized = tracker.initialize(frame, dict(zip(('x', 'y', 'w', 'h'), initial_box)), 0)
    if not initialized.get('ok'):
        raise RuntimeError(initialized)
    records, latencies, shots, states = [], [], [], Counter()
    for index, (frame, truth) in enumerate(frames, 1):
        start = time.perf_counter()
        result = tracker.update(frame, index * 1000 / 30)
        latencies.append((time.perf_counter() - start) * 1000)
        accepted = bool(result['accepted'])
        box = [result['box'][key] for key in ('x', 'y', 'w', 'h')]
        iou = overlap(box, truth) if truth is not None and accepted else 0.0
        error = (float(np.linalg.norm(np.array(box[:2]) + np.array(box[2:]) / 2 -
                                     np.array(truth[:2]) - np.array(truth[2:]) / 2))
                 if truth is not None and accepted else None)
        states[result['state']] += 1
        records.append(dict(frame=index, visible=truth is not None, truth=truth,
                            box=box, accepted=accepted, state=result['state'],
                            reason=result['reason'], iou=iou, center_error_px=error))
        if index in {1, 15, 25, 40, 55, FRAME_COUNTS[name]}:
            if truth is not None:
                x, y, w, h = map(int, truth)
                cv2.rectangle(frame, (x, y), (x + w, y + h), (255, 230, 30), 2)
            x, y, w, h = map(int, box)
            cv2.rectangle(frame, (x, y), (x + w, y + h), (30, 240, 30) if accepted else (50, 50, 255), 2)
            label = f'{name} {index}: {result["state"]} IoU={iou:.2f}'
            cv2.rectangle(frame, (0, 0), (640, 32), (15, 15, 15), -1)
            cv2.putText(frame, label, (6, 23), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 255), 1)
            shots.append(cv2.resize(frame, (480, 270)))
    visible = [r for r in records if r['visible']]
    absent = [r for r in records if not r['visible']]
    accepted = [r for r in visible if r['accepted']]
    good50 = sum(r['iou'] >= 0.5 for r in visible)
    good75 = sum(r['iou'] >= 0.75 for r in visible)
    correct_absence = sum(not r['accepted'] for r in absent)
    summary = dict(case=name, evaluated_frames=len(records), visible_frames=len(visible),
                   absent_frames=len(absent), accepted_visible_frames=len(accepted),
                   visible_success_iou50=good50 / len(visible),
                   visible_success_iou75=good75 / len(visible),
                   visible_mean_iou=float(np.mean([r['iou'] for r in visible])),
                   visible_acceptance_rate=len(accepted) / len(visible),
                   absent_false_accepts=len(absent) - correct_absence,
                   mean_accepted_center_error_px=float(np.mean([r['center_error_px'] for r in accepted])) if accepted else None,
                   frame_success_iou50_or_correct_absence=(good50 + correct_absence) / len(records),
                   mean_core_ms=float(np.mean(latencies)), states=dict(states))
    if output:
        output = Path(output)
        output.mkdir(parents=True, exist_ok=True)
        (output / f'{name}.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in records))
        if shots:
            while len(shots) % 3:
                shots.append(np.zeros_like(shots[0]))
            cv2.imwrite(str(output / f'{name}.jpg'), np.vstack([np.hstack(shots[i:i + 3]) for i in range(0, len(shots), 3)]))
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', action='store_true', help='Evaluate artifacts/baseline_precision_tracker.py')
    parser.add_argument('--module', help='Tracker module path (overrides --baseline)')
    parser.add_argument('--cases', nargs='+', choices=CASES, default=list(CASES))
    parser.add_argument('--output', default='artifacts/synthetic-current')
    args = parser.parse_args()
    module = Path(args.module) if args.module else ROOT / ('artifacts/baseline_precision_tracker.py' if args.baseline else 'precision_tracker.py')
    tracker_type = load_tracker(module)
    cv2.setNumThreads(2)
    cv2.setRNGSeed(1701)
    results = []
    for name in args.cases:
        result = evaluate_case(name, tracker_type, args.output)
        results.append(result)
        print(json.dumps(result), flush=True)
    total = sum(r['evaluated_frames'] for r in results)
    visible = sum(r['visible_frames'] for r in results)
    report = dict(module=str(module.resolve()), dataset='deterministic textured rectangle, seed 20261023',
                  note='Synthetic diagnostic only. No claim of real-world or held-out accuracy. Initialization excluded. Rejected visible frames score zero.',
                  evaluated_frames=total, visible_frames=visible,
                  visible_success_iou50=sum(r['visible_success_iou50'] * r['visible_frames'] for r in results) / visible,
                  visible_success_iou75=sum(r['visible_success_iou75'] * r['visible_frames'] for r in results) / visible,
                  visible_mean_iou=sum(r['visible_mean_iou'] * r['visible_frames'] for r in results) / visible,
                  absent_false_accepts=sum(r['absent_false_accepts'] for r in results),
                  absent_frames=sum(r['absent_frames'] for r in results),
                  frame_success_iou50_or_correct_absence=sum(r['frame_success_iou50_or_correct_absence'] * r['evaluated_frames'] for r in results) / total,
                  cases=results)
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    (output / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'cases'}, indent=2))


if __name__ == '__main__':
    main()
