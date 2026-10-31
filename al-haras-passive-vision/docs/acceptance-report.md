# Final acceptance report: AL-HARAS Passive Vision 0.1.0

NOT FOR FLIGHT OR ENGAGEMENT CONTROL.

| Item | Value |
|---|---|
| Date | 2026-10-25 |
| Tester | implementation agent: automated suites, scripted device tests over ADB, browser review |
| Release build | `0.1.0`, build ID `3d32c6249a1f5bd9`: `releases/ahpv-0.1.0-linux-armv7.tar.gz` (static musl, Zig 0.13.0) and `ahpv-0.1.0-darwin-x86_64` (host) |
| Target hardware | **Luckfox Pico Pro Max** (RV1106G3, Buildroot 2023.02.6, kernel 5.10.160), MIS5001-driver camera module, 14.6 GB vfat microSD, connected by USB-C/ADB |
| Host | macOS 13.7.8, Intel i7-7567U, Apple clang 14.0.3 |
| Device evidence | [`evidence/device-20261025T090504Z/`](../evidence/device-20261025T090504Z/README.md) |
| Host evidence | [`evidence/host-20261025T100102Z/`](../evidence/host-20261025T100102Z/summary.json) from `scripts/test-report.sh` on the release build |
| Release decision | **Not yet releasable.** Open: AC-002 FAIL (sensor identity/orientation), AC-001/AC-021 need persistent boot changes and physical power cycling, AC-013 soak, AC-014 sign-off, AC-025 |

## Rules applied

Statuses: **PASS, FAIL, BLOCKED, NOT RUN, NOT APPLICABLE.** PASS means a test was run and its
result captured, on the board where the criterion needs hardware. A host simulation counts
only where the criterion explicitly allows a fixture or simulation. Board runs used
development builds before the release build; every session manifest records its build ID
(see the device evidence README).

## Summary

| Status | Count | Criteria |
|---|---:|---|
| PASS | 18 | AC-003 to AC-009, AC-011, AC-012, AC-015 to AC-020, AC-022 to AC-024 |
| FAIL | 1 | AC-002 |
| BLOCKED | 3 | AC-001, AC-014, AC-021 |
| NOT RUN | 2 | AC-013, AC-025 |
| NOT APPLICABLE | 1 | AC-010 |

Test suites: unit 31/31 on host, host-ASan and **on the board** (release package); host
integration 10/10, fault matrix 17/17 and acceptance audits 8/8 (normal and ASan); smoke test
PASS on host and on the board.

## Acceptance table (brief §14)

| Test ID | Requirement links | Result | Evidence | Build / hardware | Defect ID |
|---|---|---|---|---|---|
| AC-001 | HWR-001, NFR-001, FR-001 | **BLOCKED** | `deploy/S99ahpv` updated for the observed vendor behaviour; boot-cycle checker ready | Needs (1) your approval to disable the vendor `rkipc` autostart and install the service, (2) 100 power cycles | D-18 |
| AC-002 | HWR-002, FR-002 | **FAIL** | Camera enumerates and captures through `/dev/video11` at the requested 1280×720 and 640×360 (`camera-frame-*.png`). But the `mis5001` driver logs "Detected **mis4001** 1311 sensor", which disagrees with the recorded specification (MIS5001), and orientation was not checked against a labelled test card | board, build 97e3ebfe6fd49b54 | D-16 |
| AC-003 | FR-016, SEC-007, NFR-007 | **PASS** | Host fixtures: FIFO stall → 503 `no_signal`, producer closed → `camera_lost`. Board: a wedged vendor 3A server stopped frame delivery; `ahpv` reported `no_signal`, stored no fabricated frames and finalized (session 1b1656c8). Physical cable removal not done (hot-unplugging MIPI is not advisable) | host + board | — |
| AC-004 | FR-009, NFR-009 | **PASS** | Board lab clip (session 640c36a8): decodable (contact sheet, stills), timing metadata in manifest/index, SHA-256 and per-frame CRC verified by `ahpv verify` **and** the independent verifier, both on the board | board, 568e11c75e85aca9 | — |
| AC-005 | FR-007, FR-008, NFR-004 | **PASS** | Board 720p overload: 300 IDs → 79 stored + 221 `queue_drop`, each once; host fault injection covers gaps, late and rejected frames | host + board | — |
| AC-006 | FR-017, FR-021 | **PASS** | Resolution changed 1280×720 → 640×360. Manifests show the driver reply (width, height, bytes/line, image size, buffers, full-range quantization, driver/card) and the *measured* rate (7.9 → 15.0 stored fps; driver reports no nominal rate) | board | — |
| AC-007 | NFR-011 | **PASS** | Identical deterministic report sections and video SHA-256 across replays | host | — |
| AC-008 | FR-005, DAT-003, DAT-004 | **PASS** | Annotation create/revise/review/delete with preserved history and independent review | host | — |
| AC-009 | DAT-006, FR-011 | **PASS** | Board reports embed board platform (kernel, device tree), build ID and executable hash, config/manifest/video hashes and measurements | board | — |
| AC-010 | §6 | **NOT APPLICABLE** | AI runtime out of scope; NPU present but unused; no model or performance claim | — | — |
| AC-011 | FR-003, FR-013 | **PASS** | Malformed, truncated, unsupported, non-video and missing inputs fail explicitly; app stays usable | host | — |
| AC-012 | FR-011, SEC-004 | **PASS** | Independent stdlib verifier passes on host artifacts and **on the board with Python 3.11.6** | host + board | — |
| AC-013 | NFR-002, NFR-003 | **NOT RUN** | 24 h soak not started (`tests/hardware/soak.sh` ready; envelope config validated for 3 min with 0 drops) | — | — |
| AC-014 | HWR-003 | **BLOCKED** | Measured: idle MemAvailable ≥158.7 MB, CPU 7.4 %; recording + preview MemAvailable ≥147.3 MB, system CPU 30.9 % mean / 42 % max, ahpv RSS ≤12.3 MB, ≤55.9 °C; CMA 66 MB fully vendor-reserved. Missing: stakeholder signature on the budget | board | — |
| AC-015 | NFR-014 | **PASS** | Byte limit and real ENOSPC (host disk image) leave existing sessions valid. Board: storage guard ran with real free-space values; a full-card test was not run on the board | host (+ board guard active) | — |
| AC-016 | FR-014, NFR-010 | **PASS** | Board with every network interface down (loopback only): capture, record, verify, report succeeded; interfaces restored (session b85059d0) | board | — |
| AC-017 | NFR-007 | **PASS** | Board: SIGKILL mid-recording → `verify` refuses an unfinalized session → `recover` (165 frames, no CRC failures, data untouched) → both verifiers pass (session 1719b47e). Host: SIGTERM, auto-recovery | host + board | — |
| AC-018 | NFR-015 | **PASS** | Release tarball installed on the board: `SHA256SUMS` all OK, binary/package build IDs match, 31/31 unit tests and smoke test pass from the installed copy | board, **3d32c6249a1f5bd9** | — |
| AC-019 | FR-015, SEC-001 | **PASS** | Board: viewer token and no token → `config set` denied (exit 6), config unchanged, both audited, chain intact. Host: full role matrix and HTTP refusals | host + board | — |
| AC-020 | NFR-012, SEC-006, SEC-003 | **PASS** (application) | Host source/binary/runtime audits; board: `ahpv` held only `/dev/video11`, its files and a 127.0.0.1 listener; no serial/bus/GPIO/PWM handles. **Open SEC-003 finding (vendor image):** telnet 23, SSH 22 and ADB-TCP 5555 listen on all interfaces | board | D-19 |
| AC-021 | HWR-005 | **BLOCKED** | Process-kill recovery passes on the board; a real power cut must be done physically by you | board | — |
| AC-022 | FR-005, SEC-005 | **PASS** | Bounds and field validation; existing annotations unchanged | host | — |
| AC-023 | FR-007, §4.1 | **PASS** | Board timestamps verified CLOCK_MONOTONIC end-of-frame (driver flags checked per buffer); driver → acquisition p50 35 ms; board wall clock unset (2021) and documented, ordering unaffected | board | D-20 |
| AC-024 | FR-018 | **PASS** | Concurrent sessions and segment rollover | host | — |
| AC-025 | NFR-015 | **NOT RUN** | Needs an engineer not involved in the implementation | — | — |

## Measured board performance (operating envelope)

Mode: 640×360 NV12, `keep_every_n: 2`, queue 32, fsync every 15 frames, vendor 3A running.
Session `s-20210101T123413Z-640c36a8`, plus 3 min and 1 min repeats (2700/2700 and 897/897 stored).

| Metric | Value |
|---|---|
| Stored rate / drops | 15.0 fps / 0 of 900 |
| Stored-frame interval | p50 67.6 ms, max 75.5 ms |
| Processing per frame | p50 5.2 ms; SD write stalls to 1.7 s absorbed (queue high-water 25/32) |
| Driver timestamp → acquisition | p50 35.1 ms, p99 36.5 ms |
| CPU (process / system) | mean 14.8 % / 20.8 % |
| Peak RSS | 12.2 MB |
| Live preview frame age | 76–160 ms (after fix F-09) |
| SD sustained write | 8.5–10.5 MB/s (64 MB synced) |
| Storage use | 311 MB per minute |

Outside the envelope (measured): 1280×720 at 30 fps stored only 7.9 fps (74 % dropped,
accounted); 640×360 at 30 fps stored 16.7 fps (44 % dropped, accounted).

## Open defect register

| ID | Severity | Item | Owner action |
|---|---|---|---|
| D-16 | High (AC-002) | Driver reports MIS4001 while the specification says MIS5001; orientation unverified | Read the module label; show a labelled test card to the camera |
| D-18 | Medium | Vendor `rkipc` autostarts and holds the camera; `S99ahpv` refuses to start while it runs | Decide whether to disable `rkipc` at boot (persistent change) |
| D-19 | High (SEC-003, vendor image) | telnetd :23 (root shell), sshd :22 and adbd TCP :5555 listen on all interfaces | Decide which to disable for the acceptance image |
| D-20 | Low | Board wall clock unset (2021-01-01) | Set RTC/NTP before acceptance recordings |
| D-01 | Medium | Raw recording limits stored rate to the envelope above | Accept the envelope, or integrate hardware encoding later |
| D-17 | Low | Log file has no rotation | Rotate externally, or add rotation |

See [known limitations](known-limitations.md) for the full list and the fixed defects
(F-01 to F-14).

## Release sign-off conditions (brief §14)

- [x] Verified actual board and installed image: **yes** (camera identity open: D-16)
- [ ] All MUST requirements implemented and tested: **mostly**; FR-001 boot autostart pending (AC-001)
- [x] All 20 core acceptance criteria have documented dispositions: **yes**
- [x] Safety and interface-isolation checks (application): **yes**; vendor listeners open (D-19)
- [ ] 24-hour soak and signed resource budget: **no**
- [x] Recording, manifest and reporting integrity validated (host and board): **yes**
- [ ] No undocumented network listeners on the image: **no** (D-19 vendor services)
- [ ] Build, installation and rollback reproduced by another engineer: **no**
- [x] Limitations, untested features and residual risks disclosed: **yes**
- [ ] Final acceptance report approved by technical reviewer: **pending**
