(() => {
  'use strict';
  class NativeVisualTracker {
    constructor(width, height) {
      this.width = width; this.height = height;
      this.session = crypto.randomUUID();
      this.frameId = 0;
      this.pending = Promise.resolve();
      this.generation = 0;
      this.disposed = false;
      this.lastTimestamp = -Infinity;
      this.result = this.empty();
    }
    empty() {
      return { box: null, state: 'SELECT TARGET', accepted: false, reason: '', confidence: 0,
        velocity: { x: 0, y: 0 }, trail: [], engine: 'NanoTrack + LK' };
    }
    get box() { return this.result.box; }
    async request(action, frame, timestamp, roi) {
      const generation = this.generation;
      const operation = async () => {
        // A reset or a new selection invalidates queued work before it reaches the server.
        if (generation !== this.generation || (this.disposed && action !== 'reset')) {
          return { ok: false, stale: true };
        }
        if (action === 'update' && timestamp <= this.lastTimestamp) return this.result;
        const frameId = String(++this.frameId);
        const response = await fetch('/api/track', {
          method: 'POST', body: frame, ...(action === 'reset' ? { keepalive: true } : {}),
          headers: { 'Content-Type': frame?.type || 'image/jpeg', 'X-Tracker-Session': this.session,
            'X-Tracker-Action': action, 'X-Timestamp-Ms': String(timestamp),
            'X-Frame-Id': frameId, ...(roi ? { 'X-Tracker-Roi': JSON.stringify(roi) } : {}) },
        });
        const result = await response.json();
        if (!response.ok || result.error) throw new Error(result.error || 'Local tracker could not process this frame');
        if (generation !== this.generation) return { ok: false, stale: true };
        if (result.frame_id !== undefined && String(result.frame_id) !== frameId) {
          throw new Error('Tracker returned a result for a different video frame');
        }
        if (action !== 'reset' && result.ok !== false) {
          this.result = result;
          this.lastTimestamp = timestamp;
        }
        return result;
      };
      const next = this.pending.then(operation);
      this.pending = next.catch(() => {});
      return next;
    }
    initialize(frame, roi, timestamp) {
      this.generation++;
      this.result = this.empty();
      this.lastTimestamp = -Infinity;
      return this.request('init', frame, timestamp, roi);
    }
    update(frame, timestamp) { return this.request('update', frame, timestamp); }
    reset() {
      this.generation++;
      this.result = this.empty();
      this.lastTimestamp = -Infinity;
      this.request('reset', null, 0).catch(() => {});
    }
    dispose() {
      this.disposed = true;
      // Queue cleanup after the running request, so a delayed init cannot recreate it.
      this.reset();
    }
    snapshot() { return this.result; }
  }
  window.NativeVisualTracker = NativeVisualTracker;
})();
