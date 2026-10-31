const test = require('node:test');
const assert = require('node:assert/strict');
const { VisualTracker } = require('../browser_tracker.js');

const width = 320, height = 180, w = 32, h = 28;
const blank = () => new Uint8Array(width * height).fill(35);
function frame(x, y, duplicate = false) {
  const pixels = blank();
  function stamp(left, top) {
    for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) {
      const value = 110 + 55 * Math.sin(i * 0.6) + 45 * Math.cos(j * 0.75) + 28 * Math.sin((i + j) * 0.4);
      pixels[(top + j) * width + left + i] = Math.max(0, Math.min(255, Math.round(value)));
    }
  }
  stamp(x, y);
  if (duplicate) stamp(x + 42, y);
  return pixels;
}

test('identical reference templates do not cancel the stationary target response', () => {
  const tracker = new VisualTracker(width, height);
  assert.equal(tracker.initialize(frame(130, 70), { x: 130, y: 70, w, h }, 0).ok, true);
  const result = tracker.update(frame(130, 70), 33);
  assert.equal(result.state, 'TRACKING');
  assert.equal(result.accepted, true);
  assert.ok(result.confidence > 0.9);
});

test('selected target follows a 16-frame translation within three working pixels', () => {
  const tracker = new VisualTracker(width, height);
  tracker.initialize(frame(130, 70), { x: 130, y: 70, w, h }, 0);
  for (let n = 1; n <= 16; n++) {
    const result = tracker.update(frame(130 + 2 * n, 70 + n), n * 33);
    const error = Math.hypot(result.box.x + result.box.w / 2 - (146 + 2 * n), result.box.y + result.box.h / 2 - (84 + n));
    assert.equal(result.accepted, true, `Target rejected at frame ${n}`);
    assert.ok(error < 3, `Center error at frame ${n}: ${error}`);
  }
  assert.ok(tracker.trail.length > 1, 'Confirmed motion must produce a visible trail');
});

test('occlusion reports loss and reappearance requires repeated confirmation', () => {
  const tracker = new VisualTracker(width, height);
  tracker.initialize(frame(130, 70), { x: 130, y: 70, w, h }, 0);
  tracker.update(frame(130, 70), 33);
  for (let n = 0; n < 7; n++) tracker.update(blank(), 66 + n * 33);
  assert.equal(tracker.state, 'LOST');
  assert.equal(tracker.accepted, false);
  assert.equal(tracker.trail.at(-1), null, 'The motion trail must break on missing visual evidence');
  const first = tracker.update(frame(130, 70), 330);
  assert.equal(first.state, 'LOST');
  tracker.update(frame(130, 70), 363);
  const confirmed = tracker.update(frame(130, 70), 396);
  assert.equal(confirmed.state, 'TRACKING');
});

test('two identical distinct targets prevent ambiguous reacquisition', () => {
  const tracker = new VisualTracker(width, height);
  tracker.initialize(frame(100, 70), { x: 100, y: 70, w, h }, 0);
  for (let n = 0; n < 6; n++) tracker.update(blank(), 33 + n * 33);
  for (let n = 0; n < 5; n++) tracker.update(frame(100, 70, true), 300 + n * 33);
  assert.equal(tracker.state, 'LOST');
  assert.equal(tracker.reason, 'AMBIGUOUS MATCH');
});

test('a featureless selection gives an actionable error', () => {
  const tracker = new VisualTracker(width, height);
  const result = tracker.initialize(blank(), { x: 40, y: 30, w, h });
  assert.equal(result.ok, false);
  assert.match(result.reason, /texture/);
  assert.equal(tracker.box, null);
});
