#!/usr/bin/env python3
"""Local camera telemetry runner for bench evaluation, without flight control."""
import argparse
import json
import logging
import math
from pathlib import Path
import sys
import threading
import time

import cv2

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tracking_runtime import LiveTrackingRuntime


class LatestFrame:
    """One frame slot: slow inference never accumulates a capture backlog."""
    def __init__(self):
        self.lock = threading.Lock()
        self.value = None
        self.sequence = 0

    def put(self, frame, timestamp):
        with self.lock:
            self.sequence += 1
            self.value = (self.sequence, frame, timestamp)

    def get(self):
        with self.lock:
            return self.value


def capture_frames(camera, mailbox, runtime, stop):
    try:
        while not stop.is_set():
            ok, frame = camera.read()
            # OpenCV supplies delivery time here, not sensor exposure time.
            delivered_at = time.monotonic()
            if not ok:
                runtime.mark_camera_failure()
                stop.set()
                break
            mailbox.put(frame, delivered_at)
    except Exception:
        logging.exception('Camera capture failed')
        runtime.mark_camera_failure()
        stop.set()
    finally:
        camera.release()


def process_frames(mailbox, runtime, roi, stop):
    last_sequence = 0
    initialized = False
    while not stop.is_set():
        item = mailbox.get()
        if item is None or item[0] == last_sequence:
            stop.wait(0.005)
            continue
        last_sequence, frame, timestamp = item
        if initialized:
            result = runtime.update(frame, timestamp)
        else:
            result = runtime.initialize(frame, roi, timestamp)
            initialized = result.get('ok', False)
        if result.get('runtime_fault'):
            stop.set()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--camera', type=int, default=0, help='Local OpenCV camera index')
    parser.add_argument('--roi', required=True, help='x,y,width,height in camera pixels')
    parser.add_argument('--max-observation-age-ms', type=float, required=True,
                        help='Bench deadline from frame delivery; choose and measure for your hardware')
    parser.add_argument('--telemetry-hz', type=float, default=20)
    parser.add_argument('--startup-timeout-s', type=float, default=10)
    args = parser.parse_args()
    try:
        values = [float(value) for value in args.roi.split(',')]
        if len(values) != 4 or not all(math.isfinite(v) for v in values) or min(values[2:]) < 12:
            raise ValueError
    except ValueError:
        parser.error('--roi requires finite x,y,width,height with width and height >= 12')
    for name in ('max_observation_age_ms', 'telemetry_hz', 'startup_timeout_s'):
        value = getattr(args, name)
        if not math.isfinite(value) or value <= 0:
            parser.error(f'--{name.replace("_", "-")} must be finite and positive')
    cv2.setNumThreads(2)
    runtime = LiveTrackingRuntime(args.max_observation_age_ms / 1000)
    camera = cv2.VideoCapture(args.camera)
    if not camera.isOpened():
        camera.release()
        parser.exit(1, 'Could not open the selected camera\n')
    mailbox, stop = LatestFrame(), threading.Event()
    workers = [threading.Thread(target=capture_frames, args=(camera, mailbox, runtime, stop), daemon=True),
               threading.Thread(target=process_frames,
                                args=(mailbox, runtime, dict(zip(('x', 'y', 'w', 'h'), values)), stop),
                                daemon=True)]
    started = time.monotonic()
    for worker in workers:
        worker.start()

    def emit():
        result = runtime.snapshot()
        result['timestamp_basis'] = 'opencv_delivery_monotonic_not_sensor_exposure'
        print(json.dumps(result, allow_nan=False), flush=True)
        return result

    try:
        while not stop.is_set():
            result = emit()
            if result['runtime_fault']:
                stop.set()
            if result['capture_timestamp_s'] is None and time.monotonic() - started >= args.startup_timeout_s:
                runtime.mark_camera_failure()
                stop.set()
            stop.wait(1 / args.telemetry_hz)
        emit()
    except (KeyboardInterrupt, BrokenPipeError):
        stop.set()
    finally:
        stop.set()
        for worker in workers:
            worker.join(timeout=1)
    return 1 if runtime.snapshot()['runtime_fault'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
