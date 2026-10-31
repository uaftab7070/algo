"""Freshness-checked perception for live cameras; emits no flight commands.

Capture timestamps are seconds in this process's time.monotonic clock domain,
recorded at acquisition (never when inference finishes). Video playback should
continue to use PrecisionTracker directly with media timestamps in milliseconds.
"""
from copy import deepcopy
import math
import logging
import threading
import time

from precision_tracker import PrecisionTracker


class LiveTrackingRuntime:
    def __init__(self, max_observation_age_s, tracker=None, clock=time.monotonic):
        if not math.isfinite(max_observation_age_s) or max_observation_age_s <= 0:
            raise ValueError('An explicit positive observation age limit is required')
        self.max_age = float(max_observation_age_s)
        self.tracker = tracker if tracker is not None else PrecisionTracker()
        self.clock = clock
        self._operation = threading.Lock()
        self._state = threading.Lock()
        self._result = deepcopy(self.tracker.snapshot())
        self._capture_time = None
        self._fault = None
        self._epoch = 0
        self._processing = False

    def _fail(self, reason):
        # Caller owns _state. A latched fault prevents an in-flight result from
        # restoring acceptance after a watchdog or explicit camera failure.
        if self._fault is None:
            self._epoch += 1
            self._fault = reason

    def mark_camera_failure(self):
        """Call on camera read failure/disconnect; reselection is required."""
        with self._state:
            self._fail('CAMERA FAILURE — RESELECT')
        return self.snapshot()

    def _snapshot_locked(self):
        now = self.clock()
        age = now - self._capture_time if self._capture_time is not None else None
        if age is not None and (age < 0 or age >= self.max_age):
            self._fail('OBSERVATION EXPIRED — RESELECT')
        result = deepcopy(self._result)
        result.update(capture_timestamp_s=self._capture_time,
                      observation_age_ms=max(0, age) * 1000 if age is not None else None,
                      max_observation_age_ms=self.max_age * 1000,
                      processing=self._processing, runtime_fault=self._fault,
                      observation_valid=bool(result.get('accepted') and
                                             age is not None and self._fault is None))
        if self._fault is not None:
            result.update(state='LOST', reason=self._fault, accepted=False,
                          ok=False,
                          confidence=0.0, velocity={'x': 0.0, 'y': 0.0},
                          measurement_source='none', flow_inliers=0, flow_ratio=0.0,
                          correlation=0.0, psr=0.0, appearance=0.0, structure=0.0,
                          recovery_confirmations=0)
            if not result['trail'] or result['trail'][-1] is not None:
                result['trail'].append(None)
        return result

    def snapshot(self):
        """Poll independently of inference; never reuse a cached accepted result.

        This lock does not cover model execution, so a consumer can observe an
        expired result even while inference is blocked in another thread.
        """
        with self._state:
            return self._snapshot_locked()

    def initialize(self, frame, roi, captured_at):
        return self._process(frame, captured_at, roi=roi, initializing=True)

    def update(self, frame, captured_at):
        return self._process(frame, captured_at)

    def _process(self, frame, captured_at, roi=None, initializing=False):
        # No frame queue: a caller must retain only its newest frame and retry
        # later if another producer owns inference.
        if not self._operation.acquire(blocking=False):
            return {**self.snapshot(), 'submitted_frame_processed': False, 'busy': True}
        try:
            with self._state:
                current = self._snapshot_locked()
                if self._fault is not None and not initializing:
                    return {**current, 'submitted_frame_processed': False}
                try:
                    captured_at = float(captured_at)
                    age = self.clock() - captured_at
                    if not math.isfinite(captured_at) or not 0 <= age < self.max_age:
                        raise ValueError
                except (TypeError, ValueError, OverflowError):
                    self._fail('INVALID OR STALE CAPTURE TIMESTAMP — RESELECT')
                    return {**self._snapshot_locked(), 'submitted_frame_processed': False}
                if not initializing:
                    if self._capture_time is None:
                        return {**current, 'submitted_frame_processed': False}
                    if captured_at <= self._capture_time:
                        return {**current, 'submitted_frame_processed': False}
                else:
                    self._epoch += 1
                    self._fault = None
                    self._capture_time = None
                    # The previous target must not be exposed while selecting.
                    self._result = dict(current, accepted=False, confidence=0.0,
                                        box=None, trail=[], state='INITIALIZING', reason='')
                epoch = self._epoch
                self._processing = True

            started = self.clock()
            failure = None
            try:
                if initializing:
                    result = self.tracker.initialize(frame, roi, captured_at * 1000)
                    if not result.get('ok'):
                        failure = 'INITIALIZATION FAILED — RESELECT'
                else:
                    result = self.tracker.update(frame, captured_at * 1000)
                if result.get('manual_required'):
                    failure = result.get('reason', 'TRACKER REQUIRES RESELECTION')
            except Exception:
                # External model/camera failures cannot leave a usable old lock.
                logging.getLogger(__name__).exception('Live tracker processing failed')
                failure = 'TRACKER FAILURE — RESELECT'
                result = None
            finished = self.clock()
            with self._state:
                self._processing = False
                if epoch == self._epoch:
                    if failure is not None:
                        self._fail(failure)
                    elif not 0 <= finished - captured_at < self.max_age:
                        self._fail('RESULT DEADLINE EXCEEDED — RESELECT')
                    else:
                        self._result = deepcopy(result)
                        self._result['processing_latency_ms'] = (finished - started) * 1000
                        self._capture_time = captured_at
                return {**self._snapshot_locked(), 'submitted_frame_processed': True}
        finally:
            self._operation.release()
