(() => {
  'use strict';

  const $ = id => document.getElementById(id);
  const video = $('video'), canvas = $('view'), ctx = canvas.getContext('2d');
  const work = document.createElement('canvas');
  const wctx = work.getContext('2d', { willReadFrequently: true });
  const displayed = document.createElement('canvas');
  const dctx = displayed.getContext('2d');
  let tracker = null, stream = null, fileURL = null, source = null;
  let frameRequest = null, frameGeneration = 0, paused = false, drag = null;
  let hasFrame = false, capturedTimestamp = 0, fps = 0, frames = 0, fpsSince = 0, latency = 0;
  let displayedTimestamp = 0, displayedResult = null, frameBusy = false;
  let selectionVersion = 0, initializing = false, resumeAfterSelection = false;
  let sourceVersion = 0;

  function message(text, error = false) {
    $('message').textContent = text;
    $('message').classList.toggle('error', error);
  }

  function capturePixels(timestamp = video.currentTime * 1000) {
    wctx.drawImage(video, 0, 0, work.width, work.height);
    capturedTimestamp = timestamp;
  }

  function presentFrame() {
    dctx.drawImage(work, 0, 0);
    displayedTimestamp = capturedTimestamp;
    displayedResult = tracker.snapshot();
    hasFrame = true;
  }

  // Lossless files preserve small targets; live capture favors a shorter frame gap.
  const encodeFrame = (frame = work) => new Promise((resolve, reject) => frame.toBlob(blob => {
    if (!blob) return reject(new Error('Could not capture the current video frame'));
    if (blob.size <= 5 * 1024 * 1024) return resolve(blob);
    // Noisy full-HD camera frames can exceed the local endpoint's image limit.
    frame.toBlob(compressed => compressed ? resolve(compressed) : reject(new Error('Could not encode this frame')),
      'image/jpeg', 0.98);
  }, source?.kind === 'CAMERA' ? 'image/jpeg' : 'image/png', 0.98));

  function cancelFrames() {
    if (frameRequest !== null) {
      if (video.cancelVideoFrameCallback) video.cancelVideoFrameCallback(frameRequest);
      else cancelAnimationFrame(frameRequest);
    }
    frameRequest = null;
  }

  function scheduleFrame() {
    if (!source || paused || frameBusy || initializing || frameRequest !== null) return;
    const generation = frameGeneration;
    const callback = async (now, metadata) => {
      frameRequest = null;
      if (generation !== frameGeneration || !source || paused) return;
      frameBusy = true;
      try {
        if (video.readyState >= 2) {
          // File playback waits for each result. Render the exact frame used for inference.
          if (source.kind === 'FILE') video.pause();
          const start = performance.now();
          const timestamp = Number.isFinite(metadata?.mediaTime) ? metadata.mediaTime * 1000 : video.currentTime * 1000;
          // requestAnimationFrame fallbacks can fire repeatedly for the same decoded frame.
          if (hasFrame && timestamp <= displayedTimestamp) return;
          capturePixels(timestamp);
          if (tracker.box && !drag) {
            const frameTracker = tracker;
            const frame = await encodeFrame();
            if (generation !== frameGeneration || !source) return;
            await frameTracker.update(frame, timestamp);
          }
          if (generation !== frameGeneration || !source) return;
          presentFrame();
          latency = performance.now() - start;
          frames++;
          if (now - fpsSince >= 500) {
            fps = frames * 1000 / (now - fpsSince);
            fpsSince = now;
            frames = 0;
          }
          render();
        }
      } catch (error) {
        if (generation === frameGeneration) {
          pause();
          message(`Preview stopped: ${error.message}`, true);
        }
      } finally {
        frameBusy = false;
        scheduleFrame();
        if (source && source.kind === 'FILE' && !paused && !initializing && !video.ended) {
          video.play().catch(error => message(error.message, true));
        }
      }
    };
    frameRequest = video.requestVideoFrameCallback ? video.requestVideoFrameCallback(callback) : requestAnimationFrame(callback);
  }

  function stopSource() {
    frameGeneration++;
    sourceVersion++;
    selectionVersion++;
    initializing = false;
    resumeAfterSelection = false;
    cancelFrames();
    if (stream) stream.getTracks().forEach(track => track.stop());
    stream = null;
    if (tracker) tracker.dispose();
    video.pause();
    video.srcObject = null;
    video.removeAttribute('src');
    video.load();
    if (fileURL) URL.revokeObjectURL(fileURL);
    fileURL = null;
    source = null;
    tracker = null;
    drag = null;
    hasFrame = false;
    displayedResult = null;
    paused = false;
    $('start').textContent = 'Start camera';
    $('start').disabled = false;
    $('pause').textContent = 'Pause';
    $('pause').disabled = $('reset').disabled = true;
    $('empty').hidden = false;
    $('timeline').hidden = true;
    ctx.clearRect(0, 0, canvas.width, canvas.height);
  }

  async function ready(kind, name, version) {
    const health = await fetch('/api/health').then(response => response.json());
    if (version !== sourceVersion) return;
    if (!health.ready) throw new Error('Start preview_server.py to enable the neural tracker');
    await video.play();
    if (version !== sourceVersion) return;
    // Render the overlay sharply even when the source video is smaller than the view.
    const scale = 1280 / video.videoWidth;
    canvas.width = Math.round(video.videoWidth * scale);
    canvas.height = Math.round(video.videoHeight * scale);
    const processingScale = Math.min(1, 1920 / video.videoWidth, 1080 / video.videoHeight);
    work.width = Math.round(video.videoWidth * processingScale);
    work.height = Math.round(video.videoHeight * processingScale);
    displayed.width = work.width;
    displayed.height = work.height;
    tracker = new NativeVisualTracker(work.width, work.height);
    source = { kind, name, width: video.videoWidth, height: video.videoHeight,
      processing_width: work.width, processing_height: work.height };
    capturePixels();
    presentFrame();
    fps = frames = latency = 0;
    fpsSince = performance.now();
    $('empty').hidden = true;
    $('pause').disabled = $('reset').disabled = false;
    $('timeline').hidden = kind !== 'FILE';
    render();
    scheduleFrame();
    message('Neural tracking ready. Drag a tight box around the object. Video playback follows the processed frames for accuracy.');
  }

  async function startCamera() {
    const deviceId = $('camera').value;
    stopSource();
    const version = sourceVersion;
    $('start').disabled = true;
    $('start').textContent = 'Opening camera…';
    message('Opening the camera. Allow access in Chrome if asked.');
    try {
      const openedStream = await navigator.mediaDevices.getUserMedia({
        video: { width: { ideal: 1280 }, height: { ideal: 720 }, frameRate: { ideal: 30 },
          ...(deviceId ? { deviceId: { exact: deviceId } } : {}) }, audio: false,
      });
      if (version !== sourceVersion) { openedStream.getTracks().forEach(track => track.stop()); return; }
      stream = openedStream;
      video.srcObject = stream;
      const track = stream.getVideoTracks()[0];
      await ready('CAMERA', track.label || 'Laptop camera', version);
      if (version !== sourceVersion) return;
      const devices = await navigator.mediaDevices.enumerateDevices();
      if (version !== sourceVersion) return;
      $('camera').replaceChildren(new Option('Default camera', ''));
      for (const device of devices.filter(d => d.kind === 'videoinput')) {
        $('camera').add(new Option(device.label || 'Camera', device.deviceId));
      }
      const activeDevice = track.getSettings().deviceId || '';
      $('camera').value = Array.from($('camera').options).some(option => option.value === activeDevice) ? activeDevice : '';
      $('start').textContent = 'Stop camera';
    } catch (error) {
      if (version !== sourceVersion) return;
      stopSource();
      message(`Camera could not start (${error.name}): ${error.message}`, true);
    } finally {
      if (version === sourceVersion || !source) $('start').disabled = false;
    }
  }

  async function openVideo(file) {
    stopSource();
    const version = sourceVersion;
    fileURL = URL.createObjectURL(file);
    video.src = fileURL;
    try {
      await ready('FILE', file.name, version);
    } catch (error) {
      if (version !== sourceVersion) return;
      stopSource();
      message(`This video could not be played: ${error.message}. Try an MP4 or WebM file.`, true);
    }
  }

  async function openSample(name = 'football', filename = 'Football.mp4') {
    stopSource();
    const version = sourceVersion;
    video.src = `/api/sample/${name}`;
    try { await ready('FILE', filename, version); }
    catch (error) { if (version === sourceVersion) { stopSource(); message(error.message, true); } }
  }

  function pause() {
    if (!source) return;
    paused = true;
    frameGeneration++;
    resumeAfterSelection = false;
    video.pause();
    cancelFrames();
    $('pause').textContent = 'Resume';
    render();
  }

  async function resume() {
    if (!source || initializing || drag) return;
    try {
      if (video.ended) {
        video.currentTime = 0;
        tracker.reset();
        displayedResult = tracker.snapshot();
        hasFrame = false;
        displayedTimestamp = 0;
      }
      await video.play();
      paused = false;
      fpsSince = performance.now();
      frames = 0;
      $('pause').textContent = 'Pause';
      scheduleFrame();
    } catch (error) { message(`Could not resume playback: ${error.message}`, true); }
  }

  function clearTarget() {
    if (!tracker) return;
    tracker.reset();
    selectionVersion++;
    initializing = false;
    frameGeneration++;
    cancelFrames();
    displayedResult = tracker.snapshot();
    drag = null;
    render();
    scheduleFrame();
    message('Target cleared. Drag a new box around the object.');
  }

  function crosshair(x, y, color, radius, unit) {
    ctx.save();
    ctx.strokeStyle = '#070b10';
    ctx.lineWidth = 6 * unit;
    ctx.beginPath();
    ctx.moveTo(x - radius, y); ctx.lineTo(x + radius, y);
    ctx.moveTo(x, y - radius); ctx.lineTo(x, y + radius);
    ctx.stroke();
    ctx.strokeStyle = color;
    ctx.lineWidth = 2 * unit;
    ctx.stroke();
    ctx.restore();
  }

  function targetBox(box, state, confirmed, confidence, unit, sx, sy, selecting = false) {
    const x = box.x * sx, y = box.y * sy, w = box.w * sx, h = box.h * sy;
    const color = selecting ? '#ecf9ff' : state === 'TRACKING' ? '#76ff35' : state === 'LOST' ? '#ff6565' : '#ffd365';
    ctx.save();
    ctx.strokeStyle = color;
    ctx.lineWidth = unit;
    if (!confirmed && !selecting) ctx.setLineDash([5 * unit, 4 * unit]);
    ctx.strokeRect(x, y, w, h);
    ctx.setLineDash([]);
    ctx.lineWidth = 3 * unit;
    const corner = Math.min(14 * unit, w / 3, h / 3);
    ctx.beginPath();
    for (const [px, py, dx, dy] of [[x,y,1,1],[x+w,y,-1,1],[x,y+h,1,-1],[x+w,y+h,-1,-1]]) {
      ctx.moveTo(px + corner * dx, py); ctx.lineTo(px, py); ctx.lineTo(px, py + corner * dy);
    }
    ctx.stroke();
    if (confirmed || selecting) crosshair(x + w / 2, y + h / 2, color, 7 * unit, unit * 0.7);
    const label = selecting ? 'SELECT TARGET' : state === 'LOST' ? 'LOST · LAST SEEN' : `CONF ${confidence.toFixed(2)}`;
    ctx.font = `bold ${14 * unit}px ui-monospace, monospace`;
    const labelY = y > 24 * unit ? y - 7 * unit : y + h + 21 * unit;
    ctx.fillStyle = '#07100bd9';
    ctx.fillRect(x - 3 * unit, labelY - 15 * unit, ctx.measureText(label).width + 6 * unit, 20 * unit);
    ctx.fillStyle = color;
    ctx.fillText(label, x, labelY);
    if (!selecting && state !== 'LOST') {
      const factor = video.videoWidth / work.width, factorY = video.videoHeight / work.height;
      ctx.fillStyle = '#e4ffdc';
      ctx.font = `${12 * unit}px ui-monospace, monospace`;
      ctx.shadowColor = '#000'; ctx.shadowBlur = 4;
      ctx.fillText(`${Math.round((box.x + box.w / 2) * factor)}, ${Math.round((box.y + box.h / 2) * factorY)}`, x, y + h + 16 * unit);
    }
    ctx.restore();
  }

  function render() {
    if (!source || video.readyState < 2 || !tracker) return;
    const width = canvas.width, height = canvas.height;
    const unit = width / 1280, sx = width / work.width, sy = height / work.height;
    const result = displayedResult || tracker.snapshot();
    ctx.clearRect(0, 0, width, height);
    ctx.drawImage(displayed, 0, 0, width, height);
    if (source.kind === 'FILE' && Number.isFinite(video.duration)) {
      const format = seconds => `${Math.floor(seconds / 60)}:${String(Math.floor(seconds % 60)).padStart(2, '0')}`;
      $('seek').value = displayedTimestamp / 1000 / Math.max(0.001, video.duration);
      $('timecode').textContent = `${format(displayedTimestamp / 1000)} / ${format(video.duration)}`;
    }
    ctx.fillStyle = 'rgba(5,13,15,.65)';
    ctx.fillRect(0, 0, 218 * unit, height);

    // Observed target history. Invalid observations split the trail into segments.
    ctx.strokeStyle = '#49caff'; ctx.lineWidth = 1.8 * unit;
    ctx.beginPath(); let connected = false;
    for (const point of result.trail) {
      if (!point) { connected = false; continue; }
      if (connected) ctx.lineTo(point.x * sx, point.y * sy);
      else ctx.moveTo(point.x * sx, point.y * sy);
      connected = true;
    }
    ctx.stroke();

    if (result.box && result.accepted && !drag) {
      const x = (result.box.x + result.box.w / 2) * sx, y = (result.box.y + result.box.h / 2) * sy;
      ctx.strokeStyle = '#ff9e57'; ctx.lineWidth = 1.5 * unit;
      ctx.beginPath(); ctx.moveTo(width / 2, height / 2); ctx.lineTo(x, y); ctx.stroke();
    }
    crosshair(width / 2, height / 2, '#b591ff', 13 * unit, unit);

    if (drag) {
      const b = dragBox();
      targetBox(b, 'INITIALIZING', false, 0, unit, sx, sy, true);
    } else if (result.box) {
      targetBox(result.box, result.state, result.accepted, result.confidence, unit, sx, sy);
    }

    const stateColor = result.state === 'TRACKING' ? '#8dff43' : result.state === 'LOST' ? '#ff7979' : '#ffd365';
    ctx.font = `bold ${16 * unit}px ui-monospace, monospace`;
    ctx.fillStyle = paused ? '#e5dcff' : stateColor;
    ctx.fillText(`STATE: ${paused ? 'PAUSED' : result.state}`, 12 * unit, 26 * unit);
    ctx.font = `${14 * unit}px ui-monospace, monospace`;
    const row = (text, y, color = '#d4e3df') => { ctx.fillStyle = color; ctx.fillText(text, 12 * unit, y * unit); };
    row(`FPS: ${fps.toFixed(1)}`, 51);
    row(`SRC: ${source.kind}`, 73);
    row(source.name.slice(0, 23), 95, '#a7b6b6');
    row(result.tracking_mode === 'compact' ? 'MATCH: CONTRAST + MOTION' : 'MATCH: NEURAL + LK', 115, '#73d8df');
    row(`TARGET: ${!result.box ? 'NONE' : result.state === 'TRACKING' ? 'LOCKED' : result.state}`, 135, stateColor);
    row(`CONF: ${result.box ? result.confidence.toFixed(2) : '—'}`, 160);
    row(`TRACK: ${latency.toFixed(1)} ms`, 184);

    const factor = video.videoWidth / work.width, factorY = video.videoHeight / work.height;
    const b = result.box;
    const cx = b ? (b.x + b.w / 2) * factor : 0;
    const cy = b ? (b.y + b.h / 2) * factorY : 0;
    row(`XY: ${b ? `${Math.round(cx)}, ${Math.round(cy)}` : '—'}`, 220, '#73d8df');
    row(`SIZE: ${b ? `${Math.round(b.w * factor)} × ${Math.round(b.h * factorY)}` : '—'}`, 244);
    row(`FLOW: ${result.flow_inliers || 0} inliers`, 266, '#acbdb8');
    const valid = result.accepted && b;
    row('IMAGE CENTER ERROR', 286, '#73d8df');
    row(`X: ${valid ? ((cx - video.videoWidth / 2) / video.videoWidth).toFixed(3) : '—'}`, 310);
    row(`Y: ${valid ? ((cy - video.videoHeight / 2) / video.videoHeight).toFixed(3) : '—'}`, 334);
    row(`dx: ${valid ? `${Math.round(cx - video.videoWidth / 2)} px` : '—'}`, 360, '#acbdb8');
    row(`dy: ${valid ? `${Math.round(cy - video.videoHeight / 2)} px` : '—'}`, 384, '#acbdb8');
    if (result.reason) {
      row(result.reason.slice(0, 24), 425, '#ffb786');
      if (result.state === 'LOST') row(result.reason.includes('SCENE CUT') || result.reason.includes('RESELECT')
        ? 'Drag to select again' : 'Searching for target…', 448, '#d6e2e1');
    }
    ctx.font = `${12 * unit}px ui-monospace, monospace`;
    ctx.fillStyle = '#e1eee8';
    ctx.shadowColor = '#000'; ctx.shadowBlur = 3;
    ctx.fillText('DRAG = select   SPACE = pause   R = clear   F = full screen', 232 * unit, height - 14 * unit);
    ctx.shadowBlur = 0;
  }

  function pointer(event) {
    const r = canvas.getBoundingClientRect();
    const scale = Math.min(r.width / canvas.width, r.height / canvas.height);
    const x0 = r.left + (r.width - canvas.width * scale) / 2;
    const y0 = r.top + (r.height - canvas.height * scale) / 2;
    return {
      x: Math.max(0, Math.min(work.width, (event.clientX - x0) * work.width / (canvas.width * scale))),
      y: Math.max(0, Math.min(work.height, (event.clientY - y0) * work.height / (canvas.height * scale))),
    };
  }

  function dragBox() {
    return { x: Math.min(drag.start.x, drag.end.x), y: Math.min(drag.start.y, drag.end.y),
      w: Math.abs(drag.end.x - drag.start.x), h: Math.abs(drag.end.y - drag.start.y) };
  }

  canvas.addEventListener('pointerdown', event => {
    if (!source || !hasFrame || initializing || event.button !== 0) return;
    const shouldResume = !paused;
    pause();
    const position = pointer(event);
    drag = { start: position, end: position, shouldResume };
    canvas.setPointerCapture(event.pointerId);
    render();
  });
  canvas.addEventListener('pointermove', event => {
    if (!drag) return;
    drag.end = pointer(event);
    render();
  });
  canvas.addEventListener('pointerup', async event => {
    if (!drag) return;
    drag.end = pointer(event);
    const shouldResume = drag.shouldResume;
    const roi = dragBox();
    drag = null;
    if (roi.w < 12 || roi.h < 12) {
      message('Draw a box at least 12 pixels wide and high around the target.', true);
      render();
      if (shouldResume) resume();
      return;
    }
    const version = ++selectionVersion;
    const selectedTracker = tracker;
    const timestamp = displayedTimestamp;
    initializing = true;
    resumeAfterSelection = shouldResume;
    try {
      message('Initializing the neural target template…');
      const frame = await encodeFrame(displayed);
      if (version !== selectionVersion || selectedTracker !== tracker) return;
      const selection = await selectedTracker.initialize(frame, roi, timestamp);
      if (version !== selectionVersion || selectedTracker !== tracker || selection.stale) return;
      displayedResult = selectedTracker.snapshot();
      message(selection.ok ? (selection.tracking_mode === 'compact'
        ? 'Small target selected. Tracking stops if the object disappears or another target makes its identity uncertain.'
        : 'Target selected. The box and line follow the visually confirmed target.') : selection.reason, !selection.ok);
    } catch (error) {
      if (version === selectionVersion) message(error.message, true);
    } finally {
      if (version === selectionVersion) {
        initializing = false;
        render();
        if (resumeAfterSelection) resume();
      }
    }
  });
  canvas.addEventListener('pointercancel', () => {
    if (!drag) return;
    const shouldResume = drag.shouldResume;
    drag = null;
    if (shouldResume) resume();
    render();
  });

  $('start').addEventListener('click', () => {
    if (stream) { stopSource(); message('Camera stopped.'); } else startCamera();
  });
  $('camera').addEventListener('change', () => { if (stream) startCamera(); });
  $('file').addEventListener('change', event => {
    const file = event.target.files[0];
    if (file) openVideo(file);
    event.target.value = '';
  });
  $('pause').addEventListener('click', () => paused ? resume() : pause());
  $('reset').addEventListener('click', clearTarget);
  $('sample').addEventListener('click', () => openSample());
  $('birds').addEventListener('click', () => openSample('birds', 'video.mp4'));
  $('seek').addEventListener('change', () => {
    if (!source || source.kind !== 'FILE' || !Number.isFinite(video.duration)) return;
    pause();
    tracker.reset();
    selectionVersion++;
    initializing = false;
    displayedResult = tracker.snapshot();
    video.currentTime = Number($('seek').value) * video.duration;
    message('Video position changed. Select the target at this frame, then resume.');
  });
  video.addEventListener('seeked', () => {
    if (source && paused && !drag) { capturePixels(); presentFrame(); render(); }
  });
  const fullscreen = () => document.fullscreenElement ? document.exitFullscreen() : $('stage').requestFullscreen();
  $('fullscreen').addEventListener('click', () => fullscreen().catch(error => message(error.message, true)));
  document.addEventListener('keydown', event => {
    if (['INPUT', 'SELECT', 'BUTTON'].includes(event.target.tagName)) return;
    if (event.code === 'Space') { event.preventDefault(); paused ? resume() : pause(); }
    if (event.key.toLowerCase() === 'r') clearTarget();
    if (event.key.toLowerCase() === 'f') fullscreen().catch(error => message(error.message, true));
  });
  video.addEventListener('ended', () => { pause(); message('Video ended. Resume to replay, or open another video.'); });
  window.addEventListener('pagehide', stopSource);
  // Read-only telemetry is also useful when checking the local preview.
  window.trackerPreview = { getState: () => ({ ...(displayedResult || {}), source, paused, fps, latency,
    timestamp_ms: displayedTimestamp, initializing }) };
  fetch('/api/health').then(response => response.json()).then(health => {
    $('sample').hidden = !health.samples?.includes('football');
    $('birds').hidden = !health.samples?.includes('birds');
    if (!health.ready) message('The neural models are missing. See README for setup.', true);
  }).catch(() => message('Start the local preview with .venv/bin/python preview_server.py to enable neural tracking.', true));
})();
