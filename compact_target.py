"""Local contrast/motion tracking for a small dark silhouette on a light background.

This is a geometric association aid, not a bird/drone classifier or reidentifier.
Missing/ambiguous evidence ends the selection rather than assigning a new identity.
"""
import math

import cv2
import numpy as np


def components(frame, region):
    x0, y0, x1, y1 = region
    gray = cv2.cvtColor(frame[y0:y1, x0:x1], cv2.COLOR_BGR2GRAY)
    if gray.size == 0:
        return [], 0, 0
    background = float(np.median(gray))
    noise = float(np.median(np.abs(gray.astype(np.float32) - background)))
    # A textured background does not satisfy this mode's observation model.
    if background < 100 or noise > 8:
        return [], background, noise
    mask = (gray.astype(np.float32) < background - max(28, 6 * noise)).astype(np.uint8)
    count, labels, stats, centers = cv2.connectedComponentsWithStats(mask)
    found = []
    for index in range(1, count):
        x, y, w, h, area = (int(v) for v in stats[index])
        if area < 4 or x == 0 or y == 0 or x + w >= gray.shape[1] or y + h >= gray.shape[0]:
            continue
        found.append({'center': centers[index] + [x0, y0],
                      'box': np.array([x + x0, y + y0, w, h], np.float32),
                      'area': area,
                      'contrast': background - float(gray[labels == index].mean())})
    return found, background, noise


class CompactTarget:
    @classmethod
    def from_selection(cls, frame, box):
        if max(box[2:]) > 96:
            return None
        height, width = frame.shape[:2]
        pad = max(12, int(max(box[2:]) * 0.6))
        region = (max(0, int(box[0]) - pad), max(0, int(box[1]) - pad),
                  min(width, int(math.ceil(box[0] + box[2])) + pad),
                  min(height, int(math.ceil(box[1] + box[3])) + pad))
        found, _, _ = components(frame, region)
        selected = [c for c in found if np.all(c['center'] >= box[:2]) and
                    np.all(c['center'] < box[:2] + box[2:]) and
                    0.005 <= c['area'] / float(np.prod(box[2:])) <= 0.85 and
                    c['contrast'] >= 40]
        if len(selected) > 1:
            raise ValueError('The selection contains multiple small targets. Select one object.')
        if not selected:
            return None
        item = selected[0]
        return cls(item, box)

    def __init__(self, item, selected_box):
        self.position = item['center'].astype(np.float64)
        self.velocity = np.zeros(2, np.float64)
        self.size = selected_box[2:].astype(np.float32).copy()
        self.area = item['area']
        self.previous_area = item['area']
        self.nearby_rival = False

    def update(self, frame, elapsed):
        if elapsed > 0.15:
            return {'reason': 'SMALL TARGET FRAME GAP — RESELECT'}
        dt = max(1 / 120, elapsed)
        diagonal = max(12, float(np.linalg.norm(self.size)))
        travel = float(np.linalg.norm(self.velocity * dt))
        predicted = self.position + self.velocity * dt
        radius = min(192, max(32, diagonal * 1.75 + travel * 2))
        height, width = frame.shape[:2]
        region = (max(0, int(predicted[0] - radius)), max(0, int(predicted[1] - radius)),
                  min(width, int(math.ceil(predicted[0] + radius))),
                  min(height, int(math.ceil(predicted[1] + radius))))
        if region[0] >= region[2] or region[1] >= region[3]:
            return {'reason': 'SMALL TARGET LEFT VIEW — RESELECT'}
        found, _, _ = components(frame, region)
        candidates = [c for c in found if max(4, self.area * 0.12) <= c['area'] <= self.area * 6 and
                      max(c['box'][2:]) <= diagonal * 3 and c['contrast'] >= 35]
        candidates.sort(key=lambda c: float(np.linalg.norm(c['center'] - predicted)))
        if not candidates:
            return {'reason': 'SMALL TARGET NOT OBSERVED — RESELECT'}
        best = candidates[0]
        residual = float(np.linalg.norm(best['center'] - predicted))
        gate = max(12, diagonal * 0.6 + travel * 0.5)
        if residual > gate:
            return {'reason': 'SMALL TARGET MOTION DISCONTINUITY — RESELECT'}
        margin = max(6, diagonal * 0.45)
        if len(candidates) > 1:
            second = float(np.linalg.norm(candidates[1]['center'] - predicted))
            separation = float(np.linalg.norm(candidates[1]['center'] - best['center']))
            if second - residual < margin or separation < margin:
                return {'reason': 'AMBIGUOUS SMALL TARGETS — RESELECT'}
            nearby_rival = separation < diagonal * 1.5
        else:
            nearby_rival = False
        if self.nearby_rival and best['area'] > self.previous_area * 1.6:
            return {'reason': 'POSSIBLE TARGET MERGE — RESELECT'}
        self.velocity = self.velocity * 0.6 + (best['center'] - self.position) / dt * 0.4
        self.position = best['center'].copy()
        self.previous_area = best['area']
        self.nearby_rival = nearby_rival
        # Retain the selected extent; wing motion must not shrink onto one edge.
        box = np.array([*(self.position - self.size / 2), *self.size], np.float32)
        contrast = float(np.clip(best['contrast'] / 128, 0, 1))
        motion = math.exp(-residual / max(1, gate))
        return {'box': box, 'velocity': self.velocity.astype(np.float32),
                'confidence': float(np.clip(0.55 + 0.25 * contrast + 0.20 * motion, 0, 1))}
