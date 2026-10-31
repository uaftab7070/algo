# Drone-following perception: implementation and release status

This repository is a manually initialized visual tracker. It is not a complete
drone-following aircraft system and has not been qualified for flight. The intended
next validation step is bench testing with recordings of a cooperative target drone.
No flight commands, pursuit controller, distance estimate, or collision avoidance
are implemented here.

The user has specified a Luckfox Pico Max, an “MSI 5001” camera (likely MIS5001;
label confirmation pending), and a DakeFPV H743. Initial tests use a cooperative
target in a controlled environment under human supervision. See the
[hardware integration plan and board inventory instructions](BOARD_INTEGRATION.md).

## Live-camera runtime

`tracking_runtime.LiveTrackingRuntime` wraps the tracker with a required, explicit
observation-age budget. It provides:

- Independent snapshot polling while model inference runs in another thread.
- Expiration when frames stop arriving, including during blocked inference.
- Rejection of inference results that arrive after the deadline.
- Latched camera, model, initialization, and timestamp faults. A fresh manual
  initialization is required to clear a fault; a late worker cannot clear it.
- Serialization of model calls with no internal frame queue. Concurrent submissions
  report `busy` and `submitted_frame_processed=false`.
- Copied snapshots so consumers cannot modify internal observations.

`observation_valid` means an accepted, unexpired visual observation. It is **not**
flight authorization, an identity guarantee, or a calibrated probability.
`box` may retain the last seen location after a fault; consumers must check
`observation_valid`. Rejected observations have no valid current target location.
Pixel velocity is image motion, not physical relative velocity.

Consumers must poll `snapshot()` independently and apply their own message timeout.
A dead process, blocked telemetry pipe, native code holding the Python interpreter,
or failed computer cannot be handled by an in-process watchdog alone. The wrapper
does not cancel a stalled native inference call; deployment needs an external
supervisor and a flight-controller-side timeout.

The wrapper accepts acquisition timestamps in **seconds from the same monotonic
clock** as the runtime. The existing core and browser preview use milliseconds in
the media timeline and remain appropriate for offline playback. Do not pass video
timestamps, wall-clock timestamps, or an unsynchronized sensor clock to the wrapper.

## Runnable camera bench test

Example only: the 200 ms budget below is an explicit test setting, not a validated
flight limit. Supply a tight ROI in the actual camera's pixel coordinates.

```sh
.venv/bin/python tools/live_tracking.py --camera 0 --roi 140,65,60,90 \
  --max-observation-age-ms 200 > observations.jsonl
```

The runner has separate capture, inference, and telemetry execution. Capture keeps
only the latest delivered frame. It emits JSON lines at 20 Hz by default and exits
with status 1 on a latched runtime fault. Ctrl-C stops the run. Restart with a new
selection after a fault. No camera is opened by the tests.

OpenCV's generic camera reader supplies **delivery timestamps**, not exposure
timestamps. Its driver may also buffer frames. The emitted `timestamp_basis`
explicitly identifies this limitation. Consequently this runner measures freshness
after delivery; it does not establish camera-to-output latency or detect a camera
that repeats an old image with a new delivery time. Hardware deployment needs a
camera adapter with exposure timestamps, frame IDs, and verified buffer behavior.

## Required before a flight release

| Item | Current status / evidence needed |
|---|---|
| Airframe, compute, camera, flight controller | Pico Max is not configured; official Buildroot recommended. H743 runs Betaflight; exact version, camera label, board revisions, SDK release, and airframe details remain to be confirmed. |
| Operating envelope | Cooperative target, controlled test environment, and human supervision confirmed. Separation, speed, and visibility limits remain to be specified. |
| Drone-specific perception accuracy | No labeled aerial-target benchmark supplied. Measure small silhouettes, crossings, multiple similar drones, blur, vibration, background clutter, occlusion, and leaving the field of view. |
| Identity continuity | Appearance recovery is heuristic. Label identity switches and false reacquisition separately from box IoU. |
| Timing | Measure exposure-to-result p50/p95/p99/max and deadline misses on the target computer, under load and thermal limits. Set the age budget from system requirements. |
| Relative geometry | Image coordinates alone do not supply reliable separation or closing speed. A validated ranging/relative-state source is still required. |
| Flight behavior | Define and test loss-of-target response, manual override, controller limits, separation constraints, and collision avoidance in the chosen flight stack. |
| Independent failure handling | Test camera disconnect, repeated frames, stalled inference, process termination, telemetry loss, computer restart, and clock mismatch. |
| Release evidence | Freeze model/dependency versions, record hardware and camera settings, then progress through replay, simulation, hardware bench tests, and controlled flight validation. |

No numeric accuracy, following distance, or speed guarantee is made. Existing
synthetic scenes verify regressions, not aerial deployment performance.

## Verification

```sh
.venv/bin/python -m unittest discover -s tests -p 'test_*.py' -v
node --test tests/native_preview.test.cjs tests/browser_tracker.test.cjs
```

Runtime tests inject a deterministic clock and model/camera faults. The native
tracker tests also run the local NanoTrack models. The labeled-video evaluator
remains `tools/benchmark_precision.py`; labels are required for accuracy claims.
