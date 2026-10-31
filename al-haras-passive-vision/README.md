# AL-HARAS Passive Vision

**NOT FOR FLIGHT OR ENGAGEMENT CONTROL.** This application captures, records,
replays and reviews camera footage, and lets people add manual region
annotations to it. It produces display overlays, recordings, logs and offline
reports only. It has no tracking, detection, lock-on, steering, flight-controller,
gimbal, serial or actuator interface, and none can be configured.

Target board: Luckfox Pico Pro Max (Rockchip RV1106G3). **Status: running on the real
board.** It captures from the camera through V4L2, records 640×360 at 15 fps with zero
dropped frames, and passes its tests on the board. Not yet releasable: the sensor identity
and orientation need confirming, and boot autostart, soak and power-cut tests are pending.
See [the acceptance report](docs/acceptance-report.md) (18 PASS, 1 FAIL, 3 BLOCKED,
2 NOT RUN, 1 N/A) and the [device evidence](evidence/device-20261025T090504Z/README.md).

## What it does

| Area | Implemented |
|---|---|
| Capture | YUV4MPEG2 file replay (deterministic, lossless), YUV4MPEG2 live stream from a pipe/FIFO, standard Linux V4L2 adapter (**verified on the board**: `/dev/video11`, NV12 full range, monotonic end-of-frame timestamps), configured temporal decimation |
| Frames | Monotonic frame IDs, acquisition vs source timestamps with an explicit basis, sequence-gap/late/regression detection, quality flags (dark, bright, flat, repeated), bounded queue with per-frame drop accounting |
| Recording | Raw planar Y4M + per-frame index (CRC-32) + event log + metrics, SHA-256 checksums, segmenting, storage reserve guard, crash recovery that never rewrites data |
| Review | Read-only browser preview (live or recorded), seek/step/play, timestamp inspection, manual ROI annotations with revisions, review and history |
| Reports | QA report JSON + CSV (deterministic and non-deterministic sections), unusable-span detection, contact sheets, stills, an independent schema verifier |
| Security | Role tokens (viewer/annotator/reviewer/operator), hash-chained and sealed audit and annotation logs, loopback-only listener (off by default), no HTTP configuration path |
| Diagnostics | `ahpv diag`, `ahpv inventory`, structured JSON logs with event IDs, resource sampling from `/proc` |

## Build and test (host)

Requires a C++17 compiler and `make`; tests need Python 3 (standard library only).

```sh
scripts/build.sh
```

```sh
scripts/test-report.sh
```

`test-report.sh` runs the unit tests, the integration, fault and acceptance suites
(normal and AddressSanitizer/UBSan builds) and the smoke test, and writes logs and
sample artifacts to `evidence/host-<UTC>/`.

## Try it

```sh
python3 tests/fixtures/make_fixtures.py tests/fixtures/generated
```

```sh
build/host/ahpv auth add --config config/host-replay.yaml --principal operator-1 --role operator
```

Save the printed token in a file with mode 600 (for example `operator.token`), then:

```sh
build/host/ahpv run --config config/host-replay.yaml --token-file operator.token --purpose "first replay" --partition development
```

With `preview.enabled: true` (as in `config/host-replay.yaml`) open
`http://127.0.0.1:8640/`, paste a token and review the session. See the
[operating guide](docs/operating-guide.md) for every command.

## Documentation

- [Architecture](docs/architecture.md) · [Data schemas and event IDs](docs/data-schemas.md) · [Security boundaries](docs/security-boundaries.md)
- [Hardware inventory](docs/hardware-inventory.md) · [Camera compatibility](docs/camera-compatibility.md) · [SDK compatibility](docs/sdk-compatibility.md)
- [Operating guide](docs/operating-guide.md) · [Maintenance guide](docs/maintenance-guide.md) · [Known limitations](docs/known-limitations.md)
- [Traceability matrix](docs/traceability-matrix.csv) · [Acceptance report](docs/acceptance-report.md) · [Requirements (SRS)](docs/SRS.md)

## Relationship to the parent folder

This project lives inside `Tracking Algo/`, whose other contents are a separate
visual-tracking prototype. This project shares **no code** with it, imports
nothing from it, and must be packaged on its own (`scripts/package.sh` packages
only this directory). The tracking code must not be deployed alongside this
application: doing so would invalidate the AC-020 interface audit.
