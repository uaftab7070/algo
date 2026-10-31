"""Local single-object NanoTrack inference with visual consistency checks.

The decoder follows NanoTrack v2 and the OpenCV implementation's tensor layout.
Unlike OpenCV's convenience tracker, confidence here excludes the position window,
and rejected candidates do not mutate the next search position or size.
Decoder reference: OpenCV 4.10 tracker_nano.cpp and HongLinChu's NanoTrack,
under Apache-2.0; see models/nanotrack/UPSTREAM_LICENSE and README.md.
"""
from dataclasses import dataclass
from pathlib import Path
from collections import deque
import math
import time

import cv2
import numpy as np


MODEL_DIR = Path(__file__).parent / 'models' / 'nanotrack'


@dataclass
class PrecisionConfig:
    min_neural_score: float = 0.48
    min_psr: float = 1.8
    min_appearance: float = 0.32
    high_confidence: float = 0.69
    lost_after_ms: float = 300.0
    recovery_frames: int = 3
    flow_max_points: int = 80
    flow_min_points: int = 5
    flow_max_fb_error: float = 1.5
    max_scale_jump: float = 1.3
    trail_points: int = 150


def center(box):
    return np.array([box[0] + box[2] / 2, box[1] + box[3] / 2], dtype=np.float32)


def rectangle(box, shape):
    height, width = shape[:2]
    x0, y0 = max(0, int(box[0])), max(0, int(box[1]))
    x1, y1 = min(width, int(math.ceil(box[0] + box[2]))), min(height, int(math.ceil(box[1] + box[3])))
    return x0, y0, max(x0, x1), max(y0, y1)


def crop_square(frame, position, side, resolution):
    side = max(4, int(round(side)))
    x = int(round(float(position[0]) - (side - 1) / 2))
    y = int(round(float(position[1]) - (side - 1) / 2))
    height, width = frame.shape[:2]
    left, top = max(0, -x), max(0, -y)
    right, bottom = max(0, x + side - width), max(0, y + side - height)
    region = frame[max(0, y):min(height, y + side), max(0, x):min(width, x + side)]
    if region.size == 0:
        return np.full((resolution, resolution, 3), 127, np.uint8)
    if left or top or right or bottom:
        region = cv2.copyMakeBorder(region, top, bottom, left, right, cv2.BORDER_CONSTANT,
                                    value=tuple(float(v) for v in frame[::32, ::32].mean(axis=(0, 1))))
    return cv2.resize(region, (resolution, resolution), interpolation=cv2.INTER_LINEAR)


class NanoMatcher:
    """Preserves the selected template; returns candidates without committing them."""
    def __init__(self):
        self.backbone = cv2.dnn.readNetFromONNX(str(MODEL_DIR / 'nanotrack_backbone_sim.onnx'))
        self.head = cv2.dnn.readNetFromONNX(str(MODEL_DIR / 'nanotrack_head_sim.onnx'))
        self.window = np.outer(np.hanning(16), np.hanning(16)).astype(np.float32)
        self.grid_x, self.grid_y = np.meshgrid((np.arange(16, dtype=np.float32) - 8) * 16 + 127,
                                             (np.arange(16, dtype=np.float32) - 8) * 16 + 127)
        self.template_features = None

    def features(self, image):
        self.backbone.setInput(cv2.dnn.blobFromImage(image, swapRB=True))
        return self.backbone.forward()

    def initialize(self, frame, box):
        context = 0.5 * (box[2] + box[3])
        side = math.sqrt((box[2] + context) * (box[3] + context))
        self.template_features = self.features(crop_square(frame, center(box), side, 127)).copy()
        self.head.setInput(self.template_features, 'input1')

    def infer(self, frame, box, expansion=1.0):
        context = 0.5 * (box[2] + box[3])
        side = math.sqrt((box[2] + context) * (box[3] + context)) * (255 / 127) * expansion
        scale = 255 / side
        features = self.features(crop_square(frame, center(box), side, 255))
        self.head.setInput(features, 'input2')
        cls, distances = self.head.forward(['output1', 'output2'])
        cls = cls.reshape(2, 16, 16)
        distances = distances.reshape(4, 16, 16)
        response = 1.0 / (1.0 + np.exp(np.clip(cls[0] - cls[1], -50, 50)))
        x1, y1 = self.grid_x - distances[0], self.grid_y - distances[1]
        x2, y2 = self.grid_x + distances[2], self.grid_y + distances[3]
        widths, heights = np.maximum(1, x2 - x1), np.maximum(1, y2 - y1)

        def size(w, h):
            padding = (w + h) * 0.5
            return np.sqrt((w + padding) * (h + padding))

        change = lambda ratio: np.maximum(ratio, 1 / np.maximum(ratio, 1e-6))
        sc = change(size(widths, heights) / size(box[2] * scale, box[3] * scale))
        rc = change((box[2] / box[3]) / (widths / heights))
        penalty = np.exp(-(sc * rc - 1) * 0.055)
        # The window ranks candidates, but is never reported as visual confidence.
        rank = penalty * response * 0.70 + self.window * 0.30
        py, px = np.unravel_index(int(rank.argmax()), rank.shape)
        raw_score = float(response[py, px])
        mask = np.ones((16, 16), bool)
        mask[max(0, py - 2):py + 3, max(0, px - 2):px + 3] = False
        sidelobes = response[mask]
        psr = float((raw_score - sidelobes.mean()) / max(0.025, float(sidelobes.std())))
        c = center(box) + np.array([(x1[py, px] + x2[py, px]) / 2 - 127,
                                    (y1[py, px] + y2[py, px]) / 2 - 127]) / scale
        learning_rate = float(penalty[py, px]) * raw_score * 0.32
        w = float(widths[py, px] / scale) * learning_rate + box[2] * (1 - learning_rate)
        h = float(heights[py, px] / scale) * learning_rate + box[3] * (1 - learning_rate)
        candidate = np.array([c[0] - w / 2, c[1] - h / 2, w, h], np.float32)
        # Identify separate locations with nearly equal evidence, excluding this peak.
        second = float(sidelobes.max())
        ambiguous = second > 0.85 and raw_score - second < 0.035
        return candidate, raw_score, psr, ambiguous


def appearance_descriptor(frame, box):
    x0, y0, x1, y1 = rectangle(box, frame.shape)
    patch = frame[y0:y1, x0:x1]
    if patch.size == 0:
        return None
    patch = cv2.resize(patch, (40, 60), interpolation=cv2.INTER_AREA)
    hsv = cv2.cvtColor(patch, cv2.COLOR_BGR2HSV)
    descriptors = []
    # Spatial color layout distinguishes a jersey/pants arrangement from a grass patch.
    for region in [hsv, hsv[:30, :20], hsv[:30, 20:], hsv[30:, :20], hsv[30:, 20:]]:
        hist = cv2.calcHist([region], [0, 1], None, [16, 8], [0, 180, 0, 256])
        cv2.normalize(hist, hist, 1.0, 0.0, cv2.NORM_L1)
        descriptors.append(hist)
    return descriptors


def appearance_similarity(a, b):
    if a is None or b is None:
        return 0.0
    scores = [1.0 - cv2.compareHist(x, y, cv2.HISTCMP_BHATTACHARYYA) for x, y in zip(a, b)]
    return float(0.5 * scores[0] + 0.125 * sum(scores[1:]))


class PrecisionTracker:
    def __init__(self, config=None):
        self.config = config or PrecisionConfig()
        self.matcher = NanoMatcher()
        self.reset()

    def reset(self):
        self.box = None
        self.previous_gray = None
        self.previous_scene = None
        self.initial_appearance = self.recent_appearance = None
        self.state, self.reason = 'SELECT TARGET', ''
        self.confidence = self.score = self.psr = 0.0
        self.accepted = False
        self.velocity = np.zeros(2, np.float32)
        self.last_timestamp = self.last_visual_timestamp = None
        self.recovery_count = self.stable = 0
        self.recovery_box = None
        self.trail = deque(maxlen=self.config.trail_points)
        self.flow_inliers = 0
        self.flow_ratio = self.appearance = 0.0
        self.manual_required = False

    def initialize(self, frame, roi, timestamp=0):
        box = np.array([roi['x'], roi['y'], roi['w'], roi['h']], np.float32)
        if not np.isfinite(box).all() or box[2] < 12 or box[3] < 12:
            return {'ok': False, 'reason': 'Draw a larger, tight box around the target.'}
        x0, y0, x1, y1 = rectangle(box, frame.shape)
        if x1 - x0 < 12 or y1 - y0 < 12:
            return {'ok': False, 'reason': 'The selection must be inside the image.'}
        box = np.array([x0, y0, x1 - x0, y1 - y0], np.float32)
        self.reset()
        self.matcher.initialize(frame, box)
        self.box = box
        self.initial_template = frame[y0:y1, x0:x1].copy()
        self.initial_appearance = appearance_descriptor(frame, box)
        self.recent_appearance = self.initial_appearance
        self.previous_gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        self.previous_scene = cv2.resize(frame, (64, 36), interpolation=cv2.INTER_AREA)
        self.last_timestamp = self.last_visual_timestamp = float(timestamp)
        self.state = 'INITIALIZING'
        self.image_shape = frame.shape[:2]
        return {'ok': True, **self.snapshot()}

    def flow(self, gray):
        if self.previous_gray is None or self.state == 'LOST':
            return None
        pad = max(20, int(max(self.box[2:]) * 0.55))
        x0, y0, x1, y1 = rectangle([self.box[0] - pad, self.box[1] - pad,
                                   self.box[2] + pad * 2, self.box[3] + pad * 2], gray.shape)
        a, b = self.previous_gray[y0:y1, x0:x1], gray[y0:y1, x0:x1]
        if a.size == 0 or a.shape != b.shape:
            return None
        mask = np.zeros(a.shape, np.uint8)
        inset_x, inset_y = self.box[2] * 0.08, self.box[3] * 0.08
        bx0, by0 = int(self.box[0] - x0 + inset_x), int(self.box[1] - y0 + inset_y)
        bx1, by1 = int(self.box[0] - x0 + self.box[2] - inset_x), int(self.box[1] - y0 + self.box[3] - inset_y)
        mask[max(0, by0):min(a.shape[0], by1), max(0, bx0):min(a.shape[1], bx1)] = 255
        points = cv2.goodFeaturesToTrack(a, self.config.flow_max_points, 0.008, 3, mask=mask)
        if points is None or len(points) < self.config.flow_min_points:
            return None
        next_points, status, error = cv2.calcOpticalFlowPyrLK(a, b, points, None, winSize=(21, 21), maxLevel=3)
        back, back_status, _ = cv2.calcOpticalFlowPyrLK(b, a, next_points, None, winSize=(21, 21), maxLevel=3)
        fb = np.linalg.norm(points.reshape(-1, 2) - back.reshape(-1, 2), axis=1)
        good = (status.ravel() == 1) & (back_status.ravel() == 1) & (error.ravel() < 25) & (fb < self.config.flow_max_fb_error)
        if int(good.sum()) < self.config.flow_min_points:
            return None
        old, new = points.reshape(-1, 2)[good], next_points.reshape(-1, 2)[good]
        matrix, inliers = cv2.estimateAffinePartial2D(old, new, method=cv2.RANSAC, ransacReprojThreshold=2.0,
                                                    maxIters=300, confidence=0.98)
        if matrix is None or inliers is None:
            return None
        count = int(inliers.sum())
        if count < self.config.flow_min_points:
            return None
        pos = center(self.box) - np.array([x0, y0])
        moved = matrix[:, :2] @ pos + matrix[:, 2] + [x0, y0]
        scale = float(np.sqrt(abs(np.linalg.det(matrix[:, :2]))))
        if not 0.8 < scale < 1.25:
            return None
        size = self.box[2:] * scale
        return {'box': np.array([*(moved - size / 2), *size], np.float32),
                'ratio': count / len(points), 'count': count}

    def update(self, frame, timestamp):
        start = time.perf_counter()
        if self.box is None:
            return self.snapshot()
        if frame.shape[:2] != self.image_shape:
            self.state, self.reason, self.accepted = 'LOST', 'SOURCE SIZE CHANGED — RESELECT', False
            self.manual_required = True
            return self.snapshot()
        timestamp = float(timestamp)
        dt = min(0.25, max(1 / 120, (timestamp - self.last_timestamp) / 1000))
        self.last_timestamp = timestamp
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        scene = cv2.resize(frame, (64, 36), interpolation=cv2.INTER_AREA)
        change = float(np.abs(scene.astype(np.float32) - self.previous_scene.astype(np.float32)).mean())
        hist_a = appearance_descriptor(self.previous_scene, [0, 0, 64, 36])
        hist_b = appearance_descriptor(scene, [0, 0, 64, 36])
        if change > 38 and appearance_similarity(hist_a, hist_b) < 0.48:
            self.manual_required = True
            self.state, self.reason = 'LOST', 'SCENE CUT — SELECT TARGET AGAIN'
        self.previous_scene = scene
        if self.manual_required:
            self.accepted, self.confidence = False, 0.0
            if not self.trail or self.trail[-1] is not None:
                self.trail.append(None)
            return self.snapshot()

        flow = self.flow(gray)
        self.flow_inliers = flow['count'] if flow else 0
        self.flow_ratio = flow['ratio'] if flow else 0.0
        predicted = self.box.copy()
        if self.state != 'LOST' and timestamp - self.last_visual_timestamp < 200:
            predicted[:2] += np.clip(self.velocity * dt, -self.box[2:] * 0.7, self.box[2:] * 0.7)
        if flow and flow['ratio'] > 0.35:
            predicted[:2] = 0.75 * flow['box'][:2] + 0.25 * predicted[:2]
        expansion = 1.45 if self.state == 'LOST' else 1.15 if self.state == 'WEAK' else 1.0
        measured, self.score, self.psr, ambiguous = self.matcher.infer(frame, predicted, expansion)
        descriptor = appearance_descriptor(frame, measured)
        original_color = appearance_similarity(self.initial_appearance, descriptor)
        recent_color = appearance_similarity(self.recent_appearance, descriptor)
        self.appearance = max(original_color, recent_color * 0.95)
        diagonal = max(1, float(np.linalg.norm(self.box[2:])))
        flow_distance = float(np.linalg.norm(center(measured) - center(flow['box']))) / diagonal if flow else None
        flow_agreement = math.exp(-flow_distance / 0.25) if flow else None
        motion_agreement = math.exp(-float(np.linalg.norm(center(measured) - center(predicted))) / diagonal)
        score_evidence = float(np.clip((self.score - 0.20) / 0.75, 0, 1))
        shape_evidence = float(np.clip(self.psr / 6, 0, 1))
        if flow:
            confidence = 0.48 * score_evidence + 0.18 * shape_evidence + 0.18 * self.appearance + 0.16 * flow_agreement
        else:
            confidence = 0.58 * score_evidence + 0.20 * shape_evidence + 0.17 * self.appearance + 0.05 * motion_agreement
        scale_change = float(np.max(np.maximum(measured[2:] / self.box[2:], self.box[2:] / np.maximum(measured[2:], 1))))
        flow_support = flow is not None and flow['ratio'] > 0.5 and flow_distance < 0.2
        valid = (self.score >= self.config.min_neural_score and self.appearance >= self.config.min_appearance and
                 (self.psr >= self.config.min_psr or flow_support) and scale_change <= self.config.max_scale_jump and
                 confidence >= 0.53 and not (ambiguous and not flow_support))
        if flow and flow['ratio'] > 0.6 and flow_distance > 0.65:
            valid = False
        self.reason = 'AMBIGUOUS MATCH' if ambiguous else 'LOW VISUAL EVIDENCE'
        if self.appearance < self.config.min_appearance:
            self.reason = 'APPEARANCE MISMATCH'
        if scale_change > self.config.max_scale_jump:
            self.reason = 'EXCESSIVE SCALE CHANGE'
        if self.state == 'LOST':
            strong = valid and self.score > 0.78 and original_color > 0.50 and self.psr > 2.5
            consistent = self.recovery_box is None or np.linalg.norm(center(measured) - center(self.recovery_box)) < diagonal * 0.4
            self.recovery_count = self.recovery_count + 1 if strong and consistent else 1 if strong else 0
            self.recovery_box = measured.copy() if strong else None
            valid = strong and self.recovery_count >= self.config.recovery_frames
            if strong and not valid:
                self.reason = 'CONFIRMING ORIGINAL TARGET'
        self.accepted = bool(valid)
        if valid:
            old_center = center(self.box)
            # Neural localization remains primary; consistent LK refines small displacements.
            if flow_support:
                neural_center = center(measured) * 0.80 + center(flow['box']) * 0.20
                measured[:2] = neural_center - measured[2:] / 2
            self.velocity = self.velocity * 0.65 + (center(measured) - old_center) / dt * 0.35
            self.box = measured
            self.last_visual_timestamp = timestamp
            self.confidence = float(np.clip(confidence, 0, 1))
            self.state = 'TRACKING' if self.confidence >= self.config.high_confidence else 'WEAK'
            self.reason = ''
            self.stable = self.stable + 1 if self.state == 'TRACKING' else 0
            if self.state == 'TRACKING':
                self.trail.append({'x': float(center(self.box)[0]), 'y': float(center(self.box)[1])})
            elif not self.trail or self.trail[-1] is not None:
                self.trail.append(None)
            if self.stable >= 12 and confidence > 0.86 and original_color > 0.6 and flow_support:
                self.recent_appearance = [a * 0.97 + b * 0.03 for a, b in zip(self.recent_appearance, descriptor)]
        else:
            self.confidence = min(0.49, confidence * 0.5)
            self.stable = 0
            self.velocity *= 0.5
            if self.state == 'LOST' or timestamp - self.last_visual_timestamp >= self.config.lost_after_ms:
                self.state = 'LOST'
            else:
                self.state = 'WEAK'
            if not self.trail or self.trail[-1] is not None:
                self.trail.append(None)
        self.previous_gray = gray if valid else None
        return {**self.snapshot(), 'latency_ms': (time.perf_counter() - start) * 1000}

    def snapshot(self):
        return {
            'box': dict(zip(('x', 'y', 'w', 'h'), (float(v) for v in self.box))) if self.box is not None else None,
            'state': self.state, 'reason': self.reason, 'confidence': float(self.confidence),
            'correlation': float(self.score), 'psr': float(self.psr), 'accepted': self.accepted,
            'velocity': {'x': float(self.velocity[0]), 'y': float(self.velocity[1])}, 'trail': list(self.trail),
            'flow_inliers': self.flow_inliers, 'flow_ratio': self.flow_ratio, 'appearance': self.appearance,
            'engine': 'NanoTrack + LK',
        }
