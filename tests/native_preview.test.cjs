const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const { webcrypto } = require('node:crypto');
const nativeCode = fs.readFileSync(require.resolve('../native_tracker.js'), 'utf8');
const previewCode = fs.readFileSync(require.resolve('../camera_preview.js'), 'utf8');
const tick = () => new Promise(resolve => setImmediate(resolve));
const response = value => ({ ok: true, json: async () => value });
const result = (options, box = { x: 20, y: 20, w: 30, h: 30 }) => ({
  ok: true, box, state: 'TRACKING', accepted: true, confidence: 0.9, trail: [],
  frame_id: options.headers['X-Frame-Id'], timestamp_ms: Number(options.headers['X-Timestamp-Ms']),
});
function nativeHarness(fetch) {
  const context = vm.createContext({ window: {}, crypto: webcrypto, fetch });
  vm.runInContext(nativeCode, context);
  return new context.window.NativeVisualTracker(320, 180);
}

test('reset invalidates a running result and cancels queued updates before sending them', async () => {
  const requests = [];
  let release;
  const tracker = nativeHarness(async (url, options) => {
    requests.push(options);
    if (options.headers['X-Tracker-Action'] === 'init') await new Promise(resolve => { release = resolve; });
    return response(result(options));
  });
  const initial = tracker.initialize({ type: 'image/png' }, { x: 1, y: 1, w: 20, h: 20 }, 0);
  await tick();
  const queued = tracker.update({}, 33);
  tracker.reset();
  release();
  assert.equal((await initial).stale, true);
  assert.equal((await queued).stale, true);
  await tracker.pending;
  assert.deepEqual(requests.map(r => r.headers['X-Tracker-Action']), ['init', 'reset']);
  assert.equal(tracker.box, null);
});

test('a new selection cannot be overwritten by the previous request', async () => {
  let release;
  const requests = [];
  const tracker = nativeHarness(async (url, options) => {
    requests.push(options);
    if (requests.length === 1) await new Promise(resolve => { release = resolve; });
    return response(result(options, JSON.parse(options.headers['X-Tracker-Roi'])));
  });
  const oldSelection = tracker.initialize({}, { x: 10, y: 10, w: 20, h: 20 }, 0);
  await tick();
  const newSelection = tracker.initialize({}, { x: 90, y: 20, w: 30, h: 30 }, 100);
  release();
  assert.equal((await oldSelection).stale, true);
  assert.equal(tracker.box, null);
  await newSelection;
  assert.equal(tracker.box.x, 90);
});

test('repeated and older decoded timestamps are not processed twice', async () => {
  const requests = [];
  const tracker = nativeHarness(async (url, options) => { requests.push(options); return response(result(options)); });
  await tracker.initialize({ type: 'image/png' }, { x: 1, y: 1, w: 20, h: 20 }, 100);
  await tracker.update({}, 100);
  await tracker.update({}, 90);
  await tracker.update({}, 133);
  assert.deepEqual(requests.map(r => r.headers['X-Timestamp-Ms']), ['100', '133']);
  assert.equal(requests[0].headers['Content-Type'], 'image/png');
});

test('dispose waits for initialization to finish before deleting the session', async () => {
  let release;
  const actions = [];
  const tracker = nativeHarness(async (url, options) => {
    const action = options.headers['X-Tracker-Action'];
    actions.push(action);
    if (action === 'init') await new Promise(resolve => { release = resolve; });
    return response(result(options));
  });
  const init = tracker.initialize({}, { x: 1, y: 1, w: 20, h: 20 }, 0);
  await tick();
  tracker.dispose();
  assert.deepEqual(actions, ['init']);
  release();
  await init;
  await tracker.pending;
  assert.deepEqual(actions, ['init', 'reset']);
  assert.equal(tracker.box, null);
});

function previewHarness() {
  const elements = new Map(), requests = [], callbacks = new Map();
  let callbackId = 0, releaseUpdate = null;
  class Element {
    constructor(id) {
      this.id = id; this.listeners = {}; this.classList = { toggle() {} }; this.value = '';
      this.width = 320; this.height = 180; this.marker = 1; this.files = [];
    }
    addEventListener(name, fn) { this.listeners[name] = fn; }
    dispatch(name, data = {}) { return this.listeners[name]?.({ target: this, ...data }); }
    getContext() {
      return new Proxy({ drawImage: image => { this.marker = image.marker; }, measureText: () => ({ width: 50 }) },
        { get: (target, name) => name in target ? target[name] : () => {} });
    }
    toBlob(callback, type) { const marker = this.marker; queueMicrotask(() => callback({ type, size: 128, marker })); }
    getBoundingClientRect() { return { left: 0, top: 0, width: 320, height: 180 }; }
    setPointerCapture() {}
    removeAttribute() {}
    load() {}
  }
  const element = id => {
    if (!elements.has(id)) elements.set(id, new Element(id));
    return elements.get(id);
  };
  const video = element('video');
  Object.assign(video, { readyState: 4, videoWidth: 320, videoHeight: 180, currentTime: 0, duration: 10,
    play: async () => {}, pause() {}, ended: false,
    requestVideoFrameCallback(fn) { const id = ++callbackId; callbacks.set(id, fn); return id; },
    cancelVideoFrameCallback(id) { callbacks.delete(id); },
  });
  const context = vm.createContext({ window: { addEventListener() {} }, crypto: webcrypto,
    document: { getElementById: element, createElement: id => new Element(id), addEventListener() {} },
    performance: { now: () => 1000 }, URL: { createObjectURL: () => 'blob:test', revokeObjectURL() {} },
    fetch: async (url, options) => {
      if (url === '/api/health') return response({ ready: true, samples: [] });
      requests.push(options);
      if (options.headers['X-Tracker-Action'] === 'update') await new Promise(resolve => { releaseUpdate = resolve; });
      return response(result(options));
    },
  });
  vm.runInContext(nativeCode, context);
  context.NativeVisualTracker = context.window.NativeVisualTracker;
  vm.runInContext(previewCode, context);
  return { element, video, requests, callbacks, context,
    release: () => releaseUpdate(),
    async open() { await element('sample').dispatch('click'); },
    async select() {
      element('view').dispatch('pointerdown', { button: 0, pointerId: 1, clientX: 20, clientY: 20 });
      element('view').dispatch('pointermove', { clientX: 50, clientY: 50 });
      await element('view').dispatch('pointerup', { pointerId: 1, clientX: 50, clientY: 50 });
      await tick();
    },
    frame(marker, mediaTime) {
      assert.equal(callbacks.size, 1);
      const [id, fn] = callbacks.entries().next().value;
      callbacks.delete(id);
      video.marker = marker;
      video.currentTime = mediaTime + 0.01;
      return fn(1100, { mediaTime });
    },
  };
}

test('selection during pending inference uses the displayed frame and its decoded timestamp', async () => {
  const h = previewHarness();
  await h.open();
  await h.select();
  assert.equal(h.requests[0].body.marker, 1);
  const processing = h.frame(2, 0.033);
  await tick();
  assert.equal(h.requests[1].headers['X-Timestamp-Ms'], '33');
  assert.equal(h.element('view').marker, 1, 'Inference must not change the displayed frame before its result');
  const selecting = h.select();
  await tick();
  assert.equal(h.element('view').marker, 1, 'Beginning selection must keep the image the user clicked');
  h.release();
  await processing;
  await selecting;
  const inits = h.requests.filter(r => r.headers['X-Tracker-Action'] === 'init');
  assert.equal(inits.length, 2);
  assert.equal(inits[1].body.marker, 1);
  assert.equal(inits[1].headers['X-Timestamp-Ms'], '0');
  assert.equal(h.callbacks.size, 1);
});

test('pause and resume cannot start another frame capture while inference is in flight', async () => {
  const h = previewHarness();
  await h.open();
  await h.select();
  const processing = h.frame(2, 0.033);
  await tick();
  h.element('pause').dispatch('click');
  await h.element('pause').dispatch('click');
  assert.equal(h.callbacks.size, 0);
  h.release();
  await processing;
  assert.equal(h.callbacks.size, 1);
  assert.equal(h.element('view').marker, 1, 'Canceled inference must not overwrite the frozen frame');
});
