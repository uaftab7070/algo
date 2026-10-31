(function (root) {
  'use strict';

  const clamp = (value, low, high) => Math.max(low, Math.min(high, value));
  const center = box => ({ x: box.x + box.w / 2, y: box.y + box.h / 2 });

  class VisualTracker {
    constructor(width, height, config = {}) {
      this.width = width;
      this.height = height;
      this.config = {
        patchSize: 20, minTexture: 6, minCorrelation: 0.58, minPsr: 2.5,
        highConfidence: 0.68, lostAfter: 5, trailLength: 150,
        updateConfidence: 0.86, updateStableFrames: 12,
        ...config,
      };
      this.reset();
    }

    reset() {
      this.box = null;
      this.initialTemplate = null;
      this.recentTemplate = null;
      this.state = 'SELECT TARGET';
      this.reason = '';
      this.confidence = 0;
      this.correlation = 0;
      this.psr = 0;
      this.accepted = false;
      this.misses = 0;
      this.stable = 0;
      this.recoveryHits = 0;
      this.recoveryBox = null;
      this.velocity = { x: 0, y: 0 };
      this.trail = [];
      this.lastTimestamp = null;
    }

    patch(gray, box) {
      const n = this.config.patchSize;
      const values = new Float32Array(n * n);
      let sum = 0;
      let k = 0;
      // Bilinear sampling avoids a response jump for subpixel target movement.
      for (let j = 0; j < n; j++) {
        const y = clamp(box.y + (j + 0.5) * box.h / n - 0.5, 0, this.height - 1);
        const y0 = Math.floor(y), y1 = Math.min(y0 + 1, this.height - 1), fy = y - y0;
        for (let i = 0; i < n; i++) {
          const x = clamp(box.x + (i + 0.5) * box.w / n - 0.5, 0, this.width - 1);
          const x0 = Math.floor(x), x1 = Math.min(x0 + 1, this.width - 1), fx = x - x0;
          const a = gray[y0 * this.width + x0] * (1 - fx) + gray[y0 * this.width + x1] * fx;
          const b = gray[y1 * this.width + x0] * (1 - fx) + gray[y1 * this.width + x1] * fx;
          values[k] = a * (1 - fy) + b * fy;
          sum += values[k++];
        }
      }
      const mean = sum / values.length;
      let squared = 0;
      for (let i = 0; i < values.length; i++) {
        values[i] -= mean;
        squared += values[i] * values[i];
      }
      return { values, norm: Math.sqrt(squared), deviation: Math.sqrt(squared / values.length) };
    }

    similarity(a, b) {
      if (a.norm < 1 || b.norm < 1) return -1;
      let dot = 0;
      for (let i = 0; i < a.values.length; i++) dot += a.values[i] * b.values[i];
      return clamp(dot / (a.norm * b.norm), -1, 1);
    }

    initialize(gray, selection, timestamp = 0) {
      const x = clamp(selection.x, 0, this.width - 1), y = clamp(selection.y, 0, this.height - 1);
      const box = { x, y, w: Math.min(selection.w, this.width - x), h: Math.min(selection.h, this.height - y) };
      if (box.w < 8 || box.h < 8) return { ok: false, reason: 'Draw a slightly larger box around the object.' };
      const reference = this.patch(gray, box);
      if (reference.deviation < this.config.minTexture) {
        return { ok: false, reason: 'This area has very little detail. Include a visible edge or texture in the box.' };
      }
      this.reset();
      this.box = box;
      this.initialTemplate = reference;
      this.recentTemplate = { values: reference.values.slice(), norm: reference.norm, deviation: reference.deviation };
      this.state = 'INITIALIZING';
      this.lastTimestamp = timestamp;
      return { ok: true };
    }

    candidate(gray, box, predicted) {
      if (box.w < 8 || box.h < 8 || box.x < 0 || box.y < 0 ||
          box.x + box.w > this.width || box.y + box.h > this.height) return null;
      const patch = this.patch(gray, box);
      const anchor = this.similarity(this.initialTemplate, patch);
      const recent = this.similarity(this.recentTemplate, patch);
      // Each location has ONE response, even when reference templates are identical.
      const score = Math.max(anchor, recent);
      const c = center(box), p = center(predicted);
      const distance = Math.hypot(c.x - p.x, c.y - p.y) / Math.max(1, Math.hypot(predicted.w, predicted.h));
      const scalePenalty = 0.02 * Math.abs(Math.log(box.w / predicted.w));
      return { ...box, score, anchor, rank: score - 0.012 * distance - scalePenalty };
    }

    match(gray, predicted) {
      const old = this.box;
      const c = center(predicted);
      const multiplier = this.state === 'LOST' ? 4.5 : this.misses ? 3.5 : 2.6;
      const radiusX = Math.max(12, old.w * (multiplier - 1) / 2);
      const radiusY = Math.max(12, old.h * (multiplier - 1) / 2);
      const step = Math.max(2, Math.floor(Math.min(old.w, old.h) / 10));
      const responses = [];
      let best = null;
      const consider = (box, collect = false) => {
        const response = this.candidate(gray, box, predicted);
        if (!response) return;
        if (collect) responses.push(response);
        if (!best || response.rank > best.rank) best = response;
      };
      // Include the predicted location exactly so a stationary target remains stationary.
      for (const scale of [1, 0.94, 1.06]) {
        const w = old.w * scale, h = old.h * scale;
        consider({ x: clamp(c.x - w / 2, 0, this.width - w), y: clamp(c.y - h / 2, 0, this.height - h), w, h });
        const x0 = Math.max(0, c.x - radiusX - w / 2), x1 = Math.min(this.width - w, c.x + radiusX - w / 2);
        const y0 = Math.max(0, c.y - radiusY - h / 2), y1 = Math.min(this.height - h, c.y + radiusY - h / 2);
        for (let y = y0; y <= y1; y += step) {
          for (let x = x0; x <= x1; x += step) consider({ x, y, w, h }, true);
        }
      }
      if (!best) return null;
      const coarse = { ...best };
      for (let dy = -step; dy <= step; dy++) {
        for (let dx = -step; dx <= step; dx++) {
          consider({ x: coarse.x + dx, y: coarse.y + dy, w: coarse.w, h: coarse.h });
        }
      }
      const peak = center(best);
      // Nearby positions and scales belong to the SAME peak, not a competing target.
      const side = responses.filter(r => {
        const p = center(r);
        return Math.abs(p.x - peak.x) > best.w * 0.65 || Math.abs(p.y - peak.y) > best.h * 0.65;
      });
      if (side.length < 8) return { ...best, psr: 0, ambiguous: true };
      const mean = side.reduce((sum, r) => sum + r.score, 0) / side.length;
      const variance = side.reduce((sum, r) => sum + (r.score - mean) ** 2, 0) / side.length;
      const other = side.reduce((a, b) => a.score > b.score ? a : b);
      let second = other.score;
      const otherCenter = center(other);
      // Refine the competing peak too: a coarse grid must not hide a second identical object.
      for (const scale of [1, 0.94, 1.06]) {
        const w = old.w * scale, h = old.h * scale;
        for (let dy = -step; dy <= step; dy++) for (let dx = -step; dx <= step; dx++) {
          const competitor = this.candidate(gray, {
            x: otherCenter.x - w / 2 + dx, y: otherCenter.y - h / 2 + dy, w, h,
          }, predicted);
          if (!competitor) continue;
          const c = center(competitor);
          if (Math.abs(c.x - peak.x) > best.w * 0.65 || Math.abs(c.y - peak.y) > best.h * 0.65) {
            second = Math.max(second, competitor.score);
          }
        }
      }
      return {
        ...best,
        psr: (best.score - mean) / Math.max(0.02, Math.sqrt(variance)),
        ambiguous: second > 0.78 && best.score - second < 0.045,
      };
    }

    addTrail(point) {
      if (point || this.trail[this.trail.length - 1]) this.trail.push(point);
      if (this.trail.length > this.config.trailLength) this.trail.splice(0, this.trail.length - this.config.trailLength);
    }

    update(gray, timestamp) {
      if (!this.box) return this.snapshot();
      const dt = clamp((timestamp - this.lastTimestamp) / 1000 || 1 / 30, 1 / 120, 0.2);
      this.lastTimestamp = timestamp;
      const predicted = { ...this.box };
      if (this.misses < 3) {
        predicted.x += clamp(this.velocity.x * dt, -this.box.w, this.box.w);
        predicted.y += clamp(this.velocity.y * dt, -this.box.h, this.box.h);
      }
      const measurement = this.match(gray, predicted);
      this.accepted = false;
      this.reason = 'LOW SIMILARITY';
      let confidence = 0;
      let valid = false;
      if (measurement) {
        this.correlation = measurement.score;
        this.psr = measurement.psr;
        const a = center(measurement), b = center(predicted);
        const agreement = Math.exp(-Math.hypot(a.x - b.x, a.y - b.y) / Math.max(1, Math.hypot(predicted.w, predicted.h)));
        confidence = 0.65 * clamp((measurement.score - 0.25) / 0.75, 0, 1) +
          0.25 * clamp((measurement.psr - 1) / 5, 0, 1) + 0.10 * agreement;
        valid = measurement.score >= this.config.minCorrelation && measurement.anchor >= 0.3 &&
          measurement.psr >= this.config.minPsr && !measurement.ambiguous && confidence >= 0.5;
        if (measurement.ambiguous || measurement.psr < this.config.minPsr) this.reason = 'AMBIGUOUS MATCH';
      } else {
        this.correlation = this.psr = 0;
        this.reason = 'NO MATCH IN SEARCH AREA';
      }
      if (this.state === 'LOST') {
        const strong = valid && confidence >= 0.78 && measurement.anchor >= 0.72;
        const near = !this.recoveryBox || Math.hypot(center(measurement || predicted).x - center(this.recoveryBox).x,
          center(measurement || predicted).y - center(this.recoveryBox).y) < Math.max(this.box.w, this.box.h) * 0.7;
        this.recoveryHits = strong ? (near ? this.recoveryHits + 1 : 1) : 0;
        this.recoveryBox = strong ? measurement : null;
        valid = strong && this.recoveryHits >= 3;
        if (strong && !valid) this.reason = 'CONFIRMING TARGET';
      }
      if (valid) {
        const oldCenter = center(this.box), nextCenter = center(measurement);
        this.velocity.x = 0.6 * this.velocity.x + 0.4 * (nextCenter.x - oldCenter.x) / dt;
        this.velocity.y = 0.6 * this.velocity.y + 0.4 * (nextCenter.y - oldCenter.y) / dt;
        this.box = { x: measurement.x, y: measurement.y, w: measurement.w, h: measurement.h };
        this.accepted = true;
        this.misses = 0;
        this.confidence = confidence;
        this.reason = '';
        this.state = confidence >= this.config.highConfidence ? 'TRACKING' : 'WEAK';
        this.stable = this.state === 'TRACKING' ? this.stable + 1 : 0;
        this.addTrail(this.state === 'TRACKING' ? nextCenter : null);
        if (this.stable >= this.config.updateStableFrames && confidence >= this.config.updateConfidence && measurement.anchor > 0.75) {
          const fresh = this.patch(gray, this.box);
          let norm = 0;
          for (let i = 0; i < fresh.values.length; i++) {
            const v = this.recentTemplate.values[i] * 0.97 + fresh.values[i] * 0.03;
            this.recentTemplate.values[i] = v;
            norm += v * v;
          }
          this.recentTemplate.norm = Math.sqrt(norm);
        }
      } else {
        this.misses++;
        this.stable = 0;
        this.confidence = Math.min(0.49, confidence * 0.5);
        this.velocity.x *= 0.5;
        this.velocity.y *= 0.5;
        this.state = this.misses >= this.config.lostAfter ? 'LOST' : 'WEAK';
        this.addTrail(null);
      }
      return this.snapshot();
    }

    snapshot() {
      return { box: this.box && { ...this.box }, state: this.state, confidence: this.confidence,
        correlation: this.correlation, psr: this.psr, accepted: this.accepted, reason: this.reason,
        velocity: { ...this.velocity }, trail: this.trail };
    }
  }

  root.VisualTracker = VisualTracker;
  if (typeof module !== 'undefined' && module.exports) module.exports = { VisualTracker };
})(typeof globalThis !== 'undefined' ? globalThis : window);
