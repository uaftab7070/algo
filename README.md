# RV1106 single-object visual tracker

C++17 implementation scaffold for manually initialized, single-object tracking. It does not detect or classify objects and emits image-space telemetry only. It contains no actuator or flight-controller interface.

## Current implementation

For the drone-following use case, see [flight readiness and the live-camera runtime](FLIGHT_READINESS.md).
The new runtime checks observation freshness and latches processing faults; the
repository remains an unqualified perception prototype, without flight control.

- Interactive rectangle selection or an explicit `x,y,width,height` ROI.
- CPU visual baseline using normalized template correlation over an adaptive local search area and three template scales.
- Initial, short-term, and recent appearance references. The initial reference is retained and never overwritten.
- Pyramidal Lucas–Kanade flow as a motion consistency check, plus a constant-velocity Kalman filter.
- Confidence fusion from correlation, response PSR, flow agreement, and predicted-position agreement.
- `TRACKING`, `WEAK`, and `LOST` states; limited prediction does not retain confirmed tracking.
- Conservative, confidence-gated short-term template updates.
- JSON-lines telemetry, optional OpenCV debug window, and an offline video/ground-truth evaluator.

This is a CPU baseline, not yet an RKNN/NanoTrack deployment. The source uses OpenCV for convenience and is intended to make the tracker behavior and evaluation interface concrete. RV1106 performance has not been measured. The implementation does not claim 25/30 FPS, successful reacquisition, or robust performance on the requested adverse conditions until those are measured.

## Build

The native NanoTrack decoder and observation state machine can be built separately
without OpenCV. See [native core build, tensor contract, and current limits](NATIVE_TRACKER.md).

Requires a C++17 compiler, CMake 3.16+, and OpenCV development files with `core`, `imgproc`, `video`, `videoio`, and `highgui` (highgui is used by the interactive CLI).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

On the Luckfox Buildroot image, build against the SDK's OpenCV or cross-compile with the SDK toolchain and sysroot. A production camera adapter should pass grayscale or BGR frames from VI/VPSS/RGA buffers into `SingleObjectTracker`; avoid copying/converting between formats multiple times. RKMPI/RKAIQ/RGA/VENC/RTSP are not wired up in this workstation scaffold.

### Browser preview (camera or local video)

The Chrome preview uses a local NanoTrack v2 neural engine, forward/backward Lucas–Kanade flow with RANSAC, and spatial color appearance checks. Run it from this folder:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-camera.txt
.venv/bin/python tools/download_models.py
.venv/bin/python preview_server.py
```

Open [the local preview](http://127.0.0.1:8765/camera_preview.html), choose **Start camera** or **Open video**, and drag a tight box around an object. The **Football sample** button is available on this laptop when its existing sample clip is present. The full-color view shows green target brackets, confidence, a purple image-center crosshair, an orange line to the observed target, a cyan trail, and tracking telemetry. A timeline allows seeking local videos; select a new target after seeking.

- **Space:** pause/resume. **R:** clear target. **F:** full screen.
- Rejected measurements break the trail. A lost target has a dashed last-seen box and no current center-to-target line.
- The original neural template features remain immutable. Appearance updates are conservative and retain the original color reference.
- Confidence combines raw neural evidence, response-map sharpness, appearance, and flow agreement. The artificial position preference is excluded from confidence.
- Inference operates on a local 255×255 search crop while target selection and flow retain up to 1920×1080 source detail. The former browser-only baseline reduced an entire football frame to 320 pixels wide.
- One frame request is in flight at a time. The overlay is drawn on the exact submitted image. File playback waits during inference; it may play slower than real time to reduce skipped frames. Camera processing takes the latest available frame.
- Lost-target recovery searches a growing local region and periodically the entire frame for distinctive saved appearances, then requires three spatially consistent confirmations. Similar competing matches are rejected. The original template is retained; up to two recent references are saved only during strongly supported tracking. A scene cut requires manual selection again.
- Reliable forward/backward flow preserves the selected box extent and bridges at most five weak neural frames when texture, color, and distributed feature points agree. A sudden texture mismatch on a previously stable object is rejected to reduce switches to similar-colored distractors.
- Selection uses exactly the displayed frame, even if inference is still running. Frame timestamps come from the decoded video frame; duplicate timestamps cannot advance recovery confirmation. File uploads use lossless PNG (JPEG fallback for frames over 5 MB); live camera uploads use JPEG quality 0.98 to limit capture gaps.
- Small dark silhouettes on a light, smooth background use local contrast and motion association automatically when the selected box is at most 96 pixels per side. This prevents a sky-dominated appearance box from continuing as a target. Missing foreground, ambiguous nearby silhouettes, possible merging, or a source-frame gap over 150 ms stops this mode and requires manual selection again. It does not automatically reidentify indistinguishable birds after loss. Other selections continue to use NanoTrack and verified flow.

The **Birds sample** button opens `~/Downloads/video.mp4` when that local file exists.
Draw a box around one bird after the initial black frames. The overlay identifies
the active matching mode. See [the birds clip diagnostic](artifacts/birds-identity-current/README.md)
for the tested selections and current limitations.

Chrome obtains camera access, then sends encoded frames only to `127.0.0.1`. `preview_server.py` performs native OpenCV inference through `precision_tracker.py`; it also serves the UI in `camera_preview.js`. No video or images are sent to an external service. This workstation runtime uses Python and CPU inference; RV1106 RKNN integration remains separate. `browser_tracker.js` is retained as the previous baseline for comparisons and is no longer the default engine.

Run native visual-evidence checks, or the previous baseline's synthetic checks:

```sh
.venv/bin/python -m unittest discover -s tests -p 'test_precision_tracker.py' -v
node --test tests/browser_tracker.test.cjs
```

The native checks cover measured flow translation, missing-target loss, scene cuts, displaced recovery after occlusion, similar-colored distractors, ambiguous reference searches, and duplicate-frame handling. Run all current Python checks with `.venv/bin/python -m unittest discover -s tests -p 'test_*.py' -v` and preview race regressions with `node --test tests/native_preview.test.cjs`. The previous baseline checks cover duplicate templates, translation, occlusion/recovery, distractors, and low-texture selection. See [the recovery improvement report](artifacts/recovery-improvements.md) for current diagnostics and limits, and [the original football comparison](artifacts/football-comparison.md) for historical measurements.

Evaluate another selection with sequential decoded frames:

```sh
.venv/bin/python tools/benchmark_precision.py /path/to/video.mp4 --start-frame 30 --frames 240 --roi 629,376,42,74
```

The output includes per-frame JSON, state counts, core processing timings, and a contact sheet. Supply `--ground-truth labels.csv` with columns `frame,x,y,width,height` to calculate IoU and center error against annotations. An optional `visible` column marks absence with `0` and blank box coordinates. The `accuracy` summary reports success at IoU >= 0.5 and >= 0.75, rejection counts, false locks, annotation coverage, and the exact denominators. Rejected visible frames count as failures. Without labels, accuracy is explicitly unmeasured. State counts and the displayed heuristic confidence score are not accuracy percentages.

Reproduce the generated motion/occlusion diagnostics using `.venv/bin/python tools/benchmark_synthetic.py --output artifacts/synthetic-improved`. Add `--baseline` to evaluate the preserved pre-change engine (use a separate output directory). These six generated sequences are development checks, not a held-out real-video evaluation. A 99% target must specify the IoU threshold, visibility rules, representative annotated clips, false-lock tolerance, and acceptable processing latency before it can be assessed.

### Python camera preview on macOS

`camera_preview.py` is the earlier standalone template/LK baseline, retained for comparison. Use the neural Chrome preview above for the improved engine. The standalone baseline requires macOS camera permission for Python/Terminal.

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-camera.txt
.venv/bin/python camera_preview.py
```

Grant camera access to Python/Terminal if macOS asks. Drag a rectangle around the target and press Enter; press `Q` or Escape to close the preview. This is a workstation preview implementation, not the C++ binary or an RV1106 performance measurement.

## Run

Interactive selection:

```sh
./build/rv1106-track --input sample.mp4
```

Explicit ROI (headless mode emits JSON lines to stdout):

```sh
./build/rv1106-track --input sample.mp4 --roi 420,180,96,120 --headless
```

The optional `--mode` is `accuracy`, `balanced`, or `low-latency`. The mode values are initial presets and should be tuned with the evaluator on the target workload.

Camera index may be supplied as a digit, for example `--input 0`. A config file is required by the CLI; `tracker.ini` is supplied. Each output line contains pixel and normalized box/center/error, velocity, confidence, state, loss reason, and tracker processing latency. `timestamp_us` is a steady-clock processing timestamp, not a hardware exposure timestamp.

## Evaluation

Ground-truth CSV format is one row per annotated frame, zero-based:

```text
frame,x,y,width,height
0,420,180,96,120
1,424,181,96,120
```

```sh
./build/rv1106-evaluate clip.mp4 420 180 96 120 labels.csv [tracker.ini]
```

The evaluator reports mean IoU, center error, lost-frame count, false-track frames (`IoU < 0.1` while not LOST), and average tracker processing latency. It currently does not calculate precision curves, recovery-time distributions, CPU/RAM/NPU use, capture-to-display latency, or per-stage platform timing. Those need to be added when the test harness and RV1106 pipeline are available.

## Performance profiles

Use the built-in mode selector and separate INI files to preserve measured configurations:

- **Accuracy:** increase search scales and flow points, retain full-resolution correlation input.
- **Balanced:** supplied `tracker.ini` starter values.
- **Low latency:** reduce `flow_max_points`, use a smaller ROI/template cap, and set `frame_skip` only after checking loss behavior.

These are tuning directions rather than validated profiles. Don't interpret placeholder thresholds as calibrated values.

## Research and benchmark status

No RV1106 board, labeled benchmark clips, or RKNN model are available in this workspace, so this repository records no measured embedded accuracy or speed. The laptop browser preview and synthetic checks are available. Run the evaluator with labeled sequences before selecting a deployment tracker. Add device-side CPU, RSS, NPU, and end-to-end timing measurements to the results.

| Tracker | Accuracy | Drift | Occlusion | Scale | CPU | NPU | RAM | FPS | Model size |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Template + LK (implemented baseline) | TBD | TBD | TBD | 3-level search | TBD | N/A | TBD | TBD | N/A |
| NanoTrack | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD | TBD |
| KCF | TBD | TBD | TBD | TBD | TBD | N/A | TBD | TBD | N/A |
| CSRT | TBD | TBD | TBD | TBD | TBD | N/A | TBD | TBD | N/A |
| MOSSE | TBD | TBD | TBD | TBD | TBD | N/A | TBD | TBD | N/A |

Optical flow is supporting evidence only. Reacquisition stays within local appearance/template matching; no detector fallback exists. The current search remains centered on the motion estimate, so full occlusion and long-range recovery require empirical validation and may need manual reinitialization.

## RV1106 next integration steps

1. Build and profile the CPU baseline on the board with labeled clips and stage timing.
2. Add an RKNN inference interface and compare a compatible NanoTrack-family model against this baseline; keep crop extraction local to the predicted ROI.
3. Validate ONNX and RKNN FP/INT8 outputs on a representative calibration set, then measure accuracy degradation and memory.
4. Add VI/VPSS frame ownership, bounded newest-frame queue, RGA crop/resize where measured useful, and RGN/VENC debug output.
5. Implement the requested metrics and adverse-condition sequences before setting performance or confidence claims.

The RKNN model path is intentionally not faked: no model, calibration data, SDK, or conversion environment was supplied.
