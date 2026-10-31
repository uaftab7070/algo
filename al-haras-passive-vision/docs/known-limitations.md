# Known limitations, open defects and residual risks

## Open items after board bring-up (2026-10-25)

| ID | Item | Impact | Next step |
|---|---|---|---|
| D-16 | Driver reports MIS4001; spec says MIS5001; orientation unverified | AC-002 FAIL | Read the module label; capture a labelled test card |
| D-18 | Vendor `rkipc` autostarts and holds the camera | Service cannot autostart (AC-001) | Your decision: disable `rkipc` at boot |
| D-19 | Vendor image exposes telnet :23 (root), SSH :22, ADB-TCP :5555 on all interfaces | SEC-003 not met for the image | Your decision: disable/limit them for acceptance |
| D-20 | Board RTC/NTP unset (wall clock 2021-01-01) | Wall dates wrong; ordering unaffected | Set the clock before dated recordings |
| L-05 | 24 h soak, 100 boot cycles, power-cut tests, signed budget not done | AC-013/AC-001/AC-021/AC-014 open | Run with your approval and physical access |
| L-06 | No authorized real-scene acceptance corpus | DAT-007 partial | Record lab scenes with permission records |
| D-21 | A run that never receives a frame still finalizes as `complete` (0 frames); `no_signal` events state the reason | Status alone can mislead | Add a dedicated status, or check `frames_stored` |
| D-22 | `ahpv` ran as root during bring-up | Least privilege not yet applied | Create user `ahpv` (group `video`) as in `deploy/S99ahpv` |
| D-23 | Vendor 3A server serves one stream session per process | Must be restarted per run | Handled in `deploy/S99ahpv`; vendor behaviour |

## Design limitations (known now)

| ID | Limitation | Mitigation / next step |
|---|---|---|
| D-01 | Raw Y4M recording: measured card rate 8.5–10.5 MB/s limits the envelope to 640×360 at 15 fps (5.2 MB/s); 720p stores only 7.9 fps | Use the envelope config; a faster card or hardware encoding widens it |
| D-02 | Preview frames are uncompressed BMP (640×360 ≈ 0.69 MB); live polling every 300 ms ≈ 2.3 MB/s | Acceptable on loopback/USB networking; add a JPEG encoder if a slower link is required |
| D-03 | The preview server handles one connection at a time with 5 s socket timeouts; a slow client can delay others by up to 5 s | Single-operator lab use; add a small worker pool if needed |
| D-04 | Integrity seals are not signatures. Truncation of the newest audit records is undetectable without an external copy | Off-board copies of audit logs; add signing if a stakeholder requires non-repudiation |
| D-05 | CRC-32 per frame detects accidental corruption only | File SHA-256 in `checksums.json` covers tampering of finalized sessions |
| D-06 | Up to `fsync_interval_frames` frames may be lost on power cut | Tune on the board against write bandwidth |
| D-07 | Session review loads the whole frame index into memory (about 350 bytes per frame) | `recording.segment_max_frames` bounds it (9000 frames ≈ 3 MB) |
| D-08 | `ahpv report` and finalization hash whole files single-threaded; time on the Cortex-A7 is unmeasured | Measure on the board; segment size bounds it |
| D-09 | Colour conversion for display assumes BT.601 limited range unless the Y4M header declares `XCOLORRANGE=FULL` | Recorded in every display transform; verify ISP output range |
| D-10 | Near-duplicate recording detection (DAT-002) is not implemented; only partition tagging and video SHA-256 identity | Compare `environment.video_sha256` across partitions; add perceptual duplicate checks if required |
| D-11 | Inter-annotator agreement is not computed | Reports say so explicitly; define a protocol before claiming agreement figures |
| D-12 | HTTP is not encrypted | Loopback bind by default plus SSH tunnelling |
| D-13 | Live stream input via pipe/FIFO carries delivery timestamps only (no exposure time) | Recorded as `stream_delivery_monotonic`; use V4L2 for driver timestamps |
| D-14 | `CMakeLists.txt` untested (CMake absent on host) | The Makefile + Zig static build is the verified path |
| D-17 | Structured log has no rotation (~17 MB/day at info level) | Keep on the SD card; rotate externally |
| D-24 | The driver reports no nominal frame rate, so "late" detection uses sequence gaps instead of interval thresholds | Set the expected rate in a future config key if needed |
| D-15 | The delete-reason prompt in the UI uses a browser dialog | Cosmetic |

## Defects found and fixed during development

| ID | Defect | Found by | Fix |
|---|---|---|---|
| F-01 | Range-for over a member of a temporary JSON value (dangling reference) dropped report flag counts | integration test (FR-022) | `flag_name_list()` returns by value; all similar loops audited; ASan build added |
| F-02 | Hash chain alone could not detect an edit of the last log record | unit test | per-record SHA-256 seal on annotation and audit records |
| F-03 | macOS `poll()` misses data arriving on a FIFO during the wait; a non-blocking FIFO open does not wake waiting writers | live-stream fault test | `select()` for stream reads; blocking FIFO open on a helper thread with timeout |
| F-04 | Rejected (corrupt) frames were also counted as missing upstream frames | fault test FR-008 | rejected frames keep sequence continuity |
| F-05 | Per-session ID in the Y4M header made identical replays produce different video hashes | AC-007 test | session ID removed from the container header |
| F-06 | `config check` opened the log file as a side effect | example-config validation | check now only parses |
| F-07 | Report schema omitted fields that the report actually contains | independent verifier | schema completed |
| F-08 | UI overlay labels scaled with tiny frames; review-only mode produced failed live requests | browser check | display-resolution drawing; status checked before polling |
| F-09 | Live preview lagged 1.2 s behind capture (published after SD writes) | board live preview | capture thread publishes a preview copy (age now 76–160 ms) |
| F-10 | Live preview assumed limited-range colour although the ISP reports full range | board live preview | colour range taken from the driver's quantization |
| F-11 | `__int128` timestamp math does not compile for 32-bit ARM | first cross build | 64-bit overflow-safe arithmetic |
| F-12 | Makefile change made `print-build-id` the default goal; flag-only changes did not rebuild objects, so embedded build IDs could go stale | package build-ID check | `.DEFAULT_GOAL := all` and a build-stamp prerequisite |
| F-13 | Test harness generated the fixture corpus lazily inside the FIFO writer, so isolated stream tests timed out | isolated test run | corpus generated up front |
| F-14 | Unbounded overflow at 640×360/30 fps on this card | board envelope runs | `pipeline.keep_every_n` declared decimation and envelope config |

## Residual risks

- **Misuse risk.** Anyone could later add a control path in a fork. Mitigation: keep
  `test_no_control_path.py` mandatory in CI and in the release checklist, and repeat the
  device audit (`tests/hardware/device_audit.sh`) on every release image.
- **Co-deployment risk.** The parent folder contains tracking code. Package and deploy
  only this project.
- **Clock risk.** Without RTC/NTP verification, wall-clock dates on the board may be
  wrong. Ordering never depends on them.
