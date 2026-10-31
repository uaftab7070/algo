# AL-HARAS — Passive Vision Research Platform
## Software Requirements Specification, Implementation Brief & Acceptance Criteria

**Document version:** 1.0  
**Date:** 24 October 2026  
**Target hardware:** Luckfox Pico Max (Rockchip RV1106G3)  
**Document status:** Engineering brief for implementation agent  
**Scope:** Camera-only, human-supervised visual observation, recording, replay, annotation, and offline evaluation

> **Mandatory scope restriction:** This project brief is for a passive visual-observation research application. It must NOT implement or expose weapon targeting, autonomous object lock-on for engagement, predictive pursuit/interception, automatic aim-point generation, target prioritization for engagement, flight-control commands, actuator commands, or any interface that converts visual observations into interception or steering instructions. The system's outputs are display overlays, diagnostics, recordings, and offline evaluation reports only.

---

# 1. Agent instructions

You are the senior embedded-vision engineer, systems architect, QA lead, and technical writer responsible for delivering this specification on **real Luckfox Pico Max hardware**.

1. Start by producing a **hardware and SDK compatibility inventory**. Do not assume an API, image sensor, firmware feature, memory reserve, or AI runtime is available without verifying it on the actual installed image and vendor documentation.
2. Produce a traceability matrix mapping every requirement (FR/NFR/SEC/DAT/OPS) to implementation modules, tests, and collected evidence.
3. Implement a reproducible, offline-capable application that captures and records camera video; allows an operator to view footage and add non-commanding annotations; supports recorded-video replay; and exports quality/performance data.
4. Prefer correct, maintainable, instrumented native components (C/C++ where supported) over heavyweight frameworks that do not fit the device. Do not add neural inference just to make the solution appear more sophisticated.
5. Keep image capture and evaluation independent from actuation or flight-control subsystems. Do not create such interfaces, even as stubs.
6. Design for resource limits, explicit failure reporting, safe shutdown/restart, and on-device profiling.
7. Deliver the code, build scripts, setup steps, documentation, test fixtures, benchmarking harness, acceptance evidence, and release package.
8. **Do not claim an acceptance criterion passes unless an automated or documented manual test was run and the result was captured.** When hardware or supported SDK components are unavailable, mark the item **BLOCKED** with exact missing prerequisites.
9. Do not fabricate measured FPS, processing latency, model compatibility, thermal behavior, image quality, tracking accuracy, or memory consumption.
10. Work in incremental, testable phases but finish with one integrated deliverable and a final consolidated acceptance report.

---

# 2. Goals and explicit non-goals

## 2.1 Goals
- Reliable image acquisition from a verified compatible camera.
- Local camera preview and camera health diagnostics.
- Local recording with correct timestamps, sequence IDs, metadata, and integrity checks.
- Human-created visual annotations for image/video study.
- Reproducible replay and **offline** review of non-weapon test scenes.
- Standardized benchmarks for the camera pipeline, data integrity, processing resource use, fault recovery, and annotation quality.
- Standalone operation without a cloud subscription.
- Clear reporting of all errors and resource limitations.

## 2.2 Out of scope / prohibited outputs
- Autonomous engagement, interception or guidance.
- Visual-to-flight-controller commands (including PID commands or setpoints).
- Predictive pursuit trajectories, lead calculations or aim-point estimates.
- Engagement decisions, target selection, weapon-facing lock status, or steering controls.
- Active sensor pointing to maintain a target in view.
- Fusion with radar or other sensors for weapon employment.
- Any hidden network, serial, MAVLink, PWM, SBUS, CAN, or similar path intended to command a vehicle or actuator.

All observation outputs must be clearly labeled **NOT FOR FLIGHT OR ENGAGEMENT CONTROL**.

---

# 3. Hardware baseline and verification

The expected board is **Luckfox Pico Max with Rockchip RV1106G3**. Published platform characteristics commonly include a single-core ARM Cortex-A7 CPU up to 1.2 GHz, an approximately 1 TOPS-class NPU, 256 MB DDR3L RAM, a MIPI CSI camera interface, local flash/microSD options, and Buildroot Linux support. **Confirm the exact SKU, available interfaces, and installed firmware before committing to technical decisions.** The published camera/ISP capabilities are NOT guaranteed end-to-end application or AI throughput.

## HWR-001 — Board identity
- Record SKU, SoC ID, board revision, bootloader, kernel, userspace image, storage partition map, and SDK version.
- **Acceptance:** Machine-readable inventory and terminal logs match the physical unit.

## HWR-002 — Camera module compatibility
- Confirm actual sensor part number, sensor module revision, cable pinout, MIPI lane support, lens, device-tree configuration, and drivers.
- Candidate examples to **check**, not assume supported in the installed build: SC3336 and MIS5001.
- **Acceptance:** Connected camera enumerates, captures, and produces correctly oriented frames on physical hardware.

## HWR-003 — Real resource budget
- Measure system memory immediately after boot, after camera initialization, during preview, and during recording.
- Check CMA/contiguous memory and any camera buffer reservation.
- **Acceptance:** A signed resource budget uses measured RAM and storage availability, not total installed RAM as a proxy.

## HWR-004 — Interfaces inventory
- Record USB, Ethernet, microSD, console/debug, camera, and thermal monitoring capabilities.
- **Acceptance:** System diagram and verified interface inventory are included in delivery.

## HWR-005 — Power and storage
- Document a stable vendor-compatible power supply, expected current draw (measured if instrumentation exists), disk capacity, and recovery after abrupt power loss.
- **Acceptance:** No filesystem damage in controlled recovery tests; limitations recorded.

---

# 4. Functional requirements

| ID | Priority | Requirement | Acceptance evidence |
|---|---|---|---|
| FR-001 | MUST | Boot the application automatically or by documented command on the physical board. | Startup log and reproducible invocation |
| FR-002 | MUST | Ingest frames from a validated compatible camera. | Camera integration test |
| FR-003 | MUST | Read supported prerecorded video files for deterministic testing. | Replay integration test |
| FR-004 | MUST | Display a read-only camera preview or recorded-video preview. | Recorded demonstration and UI test |
| FR-005 | MUST | Let a human draw, edit, save, and delete manual ROI annotations for lab footage. | Annotation UI test |
| FR-006 | MUST | Ensure annotations are strictly display/data artifacts, never commands. | Interface and code audit |
| FR-007 | MUST | Assign a monotonic frame sequence ID and acquisition timestamp to each captured frame. | Frame manifest inspection |
| FR-008 | MUST | Detect dropped, late, malformed, or corrupted frames and flag them explicitly. | Fault-injection results |
| FR-009 | MUST | Record video with a sidecar manifest containing camera and recording configuration. | Sample recording and manifest |
| FR-010 | MUST | Provide non-destructive replay with seek, pause, frame stepping if supported, and timestamp inspection. | Playback tests |
| FR-011 | MUST | Export experiment summaries as JSON and CSV with documented schemas. | Export conformance tests |
| FR-012 | MUST | Expose CPU, RAM, storage, frame delivery, processing-time, and error metrics. | Metrics sample and monitoring tests |
| FR-013 | MUST | Emit actionable human-readable and structured error messages. | Negative tests |
| FR-014 | MUST | Support offline operation for all core workflows. | Network-disconnected run |
| FR-015 | MUST | Reject unauthorized configuration changes. | Authentication/authorization test |
| FR-016 | MUST | Handle loss and recovery of the camera process without fabricated outputs. | Camera fault test |
| FR-017 | SHOULD | Select among supported, hardware-validated preview/recording resolutions. | Resolution matrix |
| FR-018 | SHOULD | Organize recordings into sessions, with unique IDs and metadata. | Session lifecycle test |
| FR-019 | SHOULD | Export contact-sheet images or representative still frames for QA review. | Review artifact test |
| FR-020 | SHOULD | Include device and runtime diagnostic page/CLI. | Operator review |
| FR-021 | SHOULD | Include firmware and configuration version in each session manifest. | Manifest validation |
| FR-022 | SHOULD | Generate a QA summary of footage quality and unusable portions. | Quality report example |

## 4.1 Camera pipeline behavioral rules
- Never silently alter the meaning of frame timestamps.
- Distinguish acquisition timestamp from processing or display time.
- Preserve original source dimensions and aspect ratio in source metadata.
- Explicitly record transformations such as resizing, rotating, color conversion, or encoding.
- Make the effective frame rate measurable rather than assuming the configured frame rate is achieved.
- Use bounded queues/buffers; document overflow/drop policy.
- Treat no-signal and low-quality scenes differently; do not fabricate substitute frames as genuine live input.

## 4.2 Human annotation data model
At minimum an annotation record contains:

```json
{
  "schema_version": "1.0",
  "session_id": "session-example",
  "frame_id": 42,
  "source_timestamp_ns": 123456789,
  "annotator_id": "reviewer-a",
  "annotation_type": "manual_region_of_interest",
  "region_xywh_pixels": [20, 30, 100, 75],
  "review_status": "unreviewed",
  "notes": "Authorized laboratory object",
  "created_at": "2026-10-24T00:00:00Z"
}
```

Values above are illustrative only. Validate image-bounds coordinates and use a consistent timestamp basis. No annotation fields may represent steering offsets, aiming solutions, or action commands.

---

# 5. Application architecture

```text
Verified Camera / Prerecorded Test Video
                    |
              Capture Adapter
                    |
          Frame Validation + IDs
                    |
       Bounded Local Frame Pipeline
                    |
       +------------+------------+
       |            |            |
   Read-Only     Recorder     Metrics/Health
    Preview          |            |
       |        Session Data       |
  Human Manual       |        Diagnostics
  Annotations        |            |
       +-------------+------------+
                     |
             Offline QA Reports
             (JSON / CSV / logs)

   [HARD ISOLATION: NO GUIDANCE OR ACTUATION PATH]
```

Use modules with defined responsibilities:
- `platform/`: board inventory, hardware abstraction, health.
- `capture/`: verified vendor-supported camera acquisition and recorded-video input.
- `frames/`: timestamps, validation, bounded buffering, transform metadata.
- `recording/`: encoded footage, manifests, indexing, file integrity.
- `annotations/`: manual annotation storage and review workflow.
- `preview/`: read-only display/streaming interface.
- `metrics/`: collection, logs, resource sampling, benchmark reports.
- `security/`: configuration authorization, audit logs, local networking boundaries.
- `tests/`: unit, integration, hardware, fault-injection, soak and acceptance tests.
- `docs/`: setup, operating guide, maintenance, limitations and acceptance evidence.

Agent must choose compatible implementations after SDK verification. Do not hardcode unverified vendor APIs or claim an unsupported neural model runs on the board.

---

# 6. Optional AI/runtime compatibility assessment

**Purpose:** assess general embedded-vision model deployment feasibility using non-weapon, non-targeting sample workloads only. AI inference is not required for release of the passive video application.

- Record the official toolchain, SDK and supported model/operator versions.
- Use a publicly redistributable, benign benchmark or simple image-quality classification sample if legally and technically compatible.
- Separate host-side conversion tests from real-device execution results.
- Record input tensor shape, model file hash, quantization format, memory usage and actual board-observed performance.
- Handle incompatible operators, input shapes and runtime initialization failures as explicit errors.
- **Do not implement target detection, automatic lock, follow, re-identification across occlusion for engagement, or intercept-related tracking under this brief.**

**Acceptance:** Compatibility report lists PASS / FAIL / NOT TESTED / BLOCKED for toolchain, conversion, load, inference, and memory usage; raw logs are included. No performance figure is published without physical-device proof.

---

# 7. Nonfunctional requirements (NFR)

The values below are **proposed QA criteria for the passive capture/evaluation application**, not guaranteed SoC capabilities or guarantees for computer-vision inference.

| ID | Category | Requirement / initial target | Verification |
|---|---|---|---|
| NFR-001 | Startup | 100/100 successful controlled boots | Automated/power-cycle log |
| NFR-002 | Reliability | 24-hour continuous capture-preview-soak with no application crash | Timestamped soak report |
| NFR-003 | Memory | No sustained unexplained RSS growth; peak fits measured available system budget | Time-series resource report |
| NFR-004 | Traceability | All recorded frames are uniquely indexed; all observed gaps or errors indicated | Manifest checker |
| NFR-005 | Fault transparency | 100% of injected capture failures reported | Fault matrix |
| NFR-006 | Preview | 30-minute uninterrupted valid-signal test with no unreported service failure | Preview session log |
| NFR-007 | Recovery | Recoverable service fault returns to safe read-only service or explicit operator action state | Fault recovery test |
| NFR-008 | Config safety | Invalid configurations rejected with actionable errors | Validation tests |
| NFR-009 | Data integrity | 100% of verification checksums match for intact saved samples | Hash verification |
| NFR-010 | Offline | Core workflows operate with LAN/WAN disabled | Isolation test |
| NFR-011 | Reproducibility | Identical replay input + build + config yields equivalent objective report fields | Replay comparison |
| NFR-012 | No control | Zero control/guidance/actuator messages or interfaces | Code/network/interface audit |
| NFR-013 | Observability | Log event IDs, software version and session ID | Log schema tests |
| NFR-014 | Storage | Low/full-storage conditions handled without corrupting existing sessions | Disk fault test |
| NFR-015 | Documentation | Another engineer can perform clean build and run from documentation | Independent reproduction |

Do not set a universal FPS, latency or model-accuracy pass bar before profiling a verified camera, SDK build, encoding method, and display path. Instead:
1. Measure observed distributions (median, p95, p99) in a controlled test.
2. Record video mode, CPU/RAM, thermal conditions, storage and network conditions.
3. Propose a hardware-backed operating envelope and have stakeholders sign it off.
4. Repeat tests after any material change.

---

# 8. Data quality, evaluation and reproducibility

## DAT-001 — Dataset provenance
Every test recording shall have a source owner/permission record, date, place category (e.g., indoor lab), sensor details, resolution, nominal FPS, and purpose.

## DAT-002 — Dataset partitioning
Keep development footage and final acceptance footage separate. Detect near-duplicate recordings where practicable, and disclose any overlap.

## DAT-003 — Reference annotation governance
Manual reference annotations shall record reviewer identity, revision, coordinate space and disagreement resolution. Annotation validity must not be inferred from subjective demonstration success alone.

## DAT-004 — Non-destructive processing
Never overwrite original recordings or original human-created annotations with processed versions without a reversible and audited change record.

## DAT-005 — Quality measurements
Report input video delivery, timestamps, corrupt frames, frame drops, encoding errors, storage integrity, annotation completeness, image-quality observations and reproducibility. If measuring annotation agreement, clearly disclose method and sample size.

## DAT-006 — Benchmark metadata
Reports must embed hardware revision, OS, SDK, executable hash, camera sensor, video mode, test conditions, input manifest hash and timestamp.

## DAT-007 — Test corpus
Provide representative authorized non-weapon scenes: static objects, manually moved objects on a test bench, variable illumination, visually cluttered scenes, empty scenes, intentionally corrupted frames, and camera-disconnected cases. These are for camera/annotation/replay reliability testing, not autonomous interception performance optimization.

---

# 9. Security and safety boundaries

| ID | Requirement | Acceptance |
|---|---|---|
| SEC-001 | Minimum necessary access for preview, recording and configuration | Role tests and configuration review |
| SEC-002 | No embedded production credentials in source code or release image | Secret scan |
| SEC-003 | All network listeners explicitly declared, documented and disabled when not needed | Port scan and config inventory |
| SEC-004 | Session manifests and reports contain required integrity indicators | Manifest verifier |
| SEC-005 | No executable or network action from untrusted annotation fields | Input validation tests |
| SEC-006 | No control-interface capability, including hidden/debug functionality | Source, process, device, interface and network audit |
| SEC-007 | All operational errors are logged; UI never presents missing frames as valid current data | Fault injection |
| SEC-008 | Recording retention and operator permissions documented | Policy walkthrough |

The system must not connect vision outputs to navigation, mission planning, gimbals, autopilots, radio-control receivers, or any system capable of directing vehicle motion or weapon effects.

---

# 10. Fault-handling matrix

| Fault | Required response | Expected evidence |
|---|---|---|
| Camera absent at boot | Clear initialization failure, no fabricated footage | Boot/fault log |
| Camera interrupted during operation | Show disconnected state, log event, recover only via safe camera service | Fault test |
| Corrupted frame | Mark/reject invalid frame without pretending it is valid input | Frame error log |
| Frame queue overflow | Apply documented bounded queue policy; count every drop | Queue metrics |
| Video decoder error | Explicit error, preserve other session data | Replay negative test |
| Invalid ROI coordinates | Reject annotation and explain valid bounds | UI/API validation result |
| Storage near full | Warn and protect existing recordings | Disk pressure test |
| Disk completely full | Stop new writes safely; avoid corrupting existing sessions | Filesystem validation |
| Loss of network | Local operation continues; remote display reports unavailable | Offline test |
| High memory pressure | Log pressure and fail safely rather than deadlock or fabricated output | Memory fault test |
| Unexpected power loss | On restart, report recovery state and preserve completed artifacts | Recovery test |
| Unsupported runtime/model | Reject with diagnostic and no unsupported performance claim | Compatibility test |
| Unauthorized configuration change | Deny and audit change | Security test |

---

# 11. Acceptance criteria — executable QA catalogue

For each test below, provide **test ID, purpose, prerequisites, precise procedure, expected behavior, observed behavior, PASS/FAIL/BLOCKED, date, tester, device/firmware version, and evidence link/path**. Required tests may not be waived without explicit written sign-off.

### Group A: hardware and camera

**AC-001 — Boot validation**  
Run 100 documented controlled boot cycles. Pass only if all boots reach the expected application-ready state and logs are retained.

**AC-002 — Camera initialization**  
Connect the chosen and verified camera. Pass only if actual sensor identity, image dimensions, orientation and test capture match the recorded specification.

**AC-003 — Camera loss**  
Remove camera input or simulate a camera-service disconnect using a safe test fixture. Pass if the application reports loss and does not display stale imagery as live.

**AC-004 — Recording file integrity**  
Record a laboratory test clip; verify decodability, expected timing metadata and checksum. Pass if all checks succeed.

**AC-005 — Frame accounting**  
Inspect the frame manifest. Pass if every emitted frame has a unique ID and all observed discontinuities or errors have explicit markers.

### Group B: replay, annotations, evaluation

**AC-006 — Effective settings reporting**  
Change a permitted camera/recording setting. Pass if the displayed settings and session manifest show the actual active configuration, not merely the requested value.

**AC-007 — Replay reproducibility**  
Run the same immutable prerecorded test twice using the same software and settings. Pass if defined deterministic report fields match and any non-deterministic differences are documented.

**AC-008 — Reference data governance**  
Create, review and revise manual ROI annotations. Pass if annotator, revision, coordinate schema and review status are preserved.

**AC-009 — Benchmark provenance**  
Generate a benchmarking report. Pass if device, build, input corpus, runtime configuration and measurements are fully traceable.

**AC-010 — Unsupported AI runtime case**  
Attempt to load a demonstrably unsupported benign sample model if AI runtime testing is in scope. Pass if a clear error is returned with no crash or fabricated score. If AI is omitted, mark NOT APPLICABLE with rationale.

**AC-011 — Corrupt input handling**  
Feed invalid, truncated and unsupported video test files. Pass if failures are explicit and the application remains usable.

**AC-012 — Report schema conformance**  
Export CSV and JSON. Pass if files conform to published schemas and required report fields can be parsed by an independent verifier.

### Group C: operational reliability

**AC-013 — Soak test**  
Run a 24-hour capture/preview/record test on physical hardware. Pass if no application crash occurs and resource trends and faults are documented.

**AC-014 — Resource profiling**  
Measure boot, camera-ready, preview and recording states. Pass if CPU/RAM/storage usage distributions and measured peaks are included in a signed budget.

**AC-015 — Storage failure**  
Safely simulate low and exhausted recording-space conditions. Pass if existing files remain valid, errors are visible and new writes stop safely when needed.

**AC-016 — Offline operation**  
Disable all network connectivity. Pass if local camera, preview (where local), recording, replay and reporting work as documented.

**AC-017 — Recoverable failure**  
Safely interrupt an application camera/recording service. Pass if recovery follows documented behavior, emits diagnostics and never implies missing footage was captured.

**AC-018 — Reproducible release**  
Install an approved release image/build again on the device. Pass if documented setup and regression tests work without undocumented manual fixes.

**AC-019 — Unauthorized configuration**  
Attempt prohibited setting changes using a non-privileged account. Pass if denied and logged.

**AC-020 — No actuation or guidance path**  
Inspect source, binary dependencies, processes, network sockets, configured serial/device interfaces, UI and logs. Pass only if no capture/annotation/evaluation output can produce autonomous flight, steering, gimbal, target-selection or interceptor commands.

### Additional recommended tests

**AC-021 — Abrupt restart:** verify recoverability and session-integrity status after a controlled interrupted-power scenario.  
**AC-022 — Annotation bounds:** reject invalid coordinates and retain existing reference annotations unchanged.  
**AC-023 — Clock/timestamp integrity:** document the monotonic and wall-clock timestamp relationship; ensure correct ordering in saved manifests.  
**AC-024 — Session collision:** generate multiple sessions and verify unique IDs, isolation and complete metadata.  
**AC-025 — Documentation reproducibility:** an engineer not involved in implementation performs a clean build and an end-to-end recording/export using only the written guide.

---

# 12. Implementation phases and gates

## Phase 0 — Discovery and baseline
**Produce:** hardware inventory, firmware/SDK verification, camera selection evidence, partition layout, measured free RAM, architecture decision record.  
**Gate:** HWR-001 through HWR-005 approved; unknowns listed, not silently assumed.

## Phase 1 — Camera capture and reliable recording
**Produce:** camera driver integration, preview, frame IDs/timestamps, bounded frame pipeline, local recording, integrity manifest.  
**Gate:** AC-002, AC-004, AC-005 demonstrably pass.

## Phase 2 — Human observation and evaluation
**Produce:** read-only preview, manual ROI annotation workflow, replay, dataset/annotation storage, CSV/JSON reporting.  
**Gate:** AC-006, AC-007, AC-008, AC-009, AC-012 pass.

## Phase 3 — Robustness and security
**Produce:** explicit error handling, authorization, low-storage handling, network isolation, logging and recovery tests.  
**Gate:** AC-003, AC-011, AC-015, AC-016, AC-017, AC-019, AC-020 pass.

## Phase 4 — Hardening and acceptance
**Produce:** resource envelope, 24-hour soak, release package, regression suite, installation guide, complete QA evidence and signed review.  
**Gate:** All MUST requirements and required AC tests pass, or any exceptional deviations receive written approval.

---

# 13. Source repository and required deliverable structure

```text
al-haras-passive-vision/
  README.md
  docs/
    SRS.md
    hardware-inventory.md
    architecture.md
    camera-compatibility.md
    sdk-compatibility.md
    operating-guide.md
    maintenance-guide.md
    data-schemas.md
    security-boundaries.md
    known-limitations.md
    traceability-matrix.csv
    acceptance-report.md
  src/
    platform/
    capture/
    frames/
    recording/
    annotations/
    preview/
    metrics/
    security/
  config/
    example-config.yaml
  schemas/
    session-manifest.schema.json
    annotation.schema.json
    qa-report.schema.json
  tests/
    unit/
    integration/
    hardware/
    faults/
    acceptance/
    fixtures/
  scripts/
    build.sh
    package.sh
    smoke-test.sh
    test-report.sh
  releases/
    README.md
```

This is a proposed structure; the agent may adapt it but shall preserve the specified deliverables and separation between non-actuating observation components.

---

# 14. Final acceptance report format

Provide this table as a completed artifact, not an empty template:

| Test ID | Requirement links | Result | Evidence | Build / hardware | Defect ID |
|---|---|---|---|---|---|
| AC-001 | HWR-001, NFR-001 | NOT RUN | — | — | — |
| AC-002 | HWR-002, FR-002 | NOT RUN | — | — | — |
| ... | ... | ... | ... | ... | ... |

Allowed statuses: **PASS, FAIL, BLOCKED, NOT RUN, NOT APPLICABLE**. Distinguish lack of a physical board from successful simulation. The agent must maintain an open-defect register and explain every FAIL/BLOCKED/NOT RUN item.

## Release sign-off conditions
- [ ] Verified actual board, camera and installed SDK.
- [ ] All MUST requirements implemented and tested.
- [ ] All 20 core acceptance criteria have documented dispositions.
- [ ] All safety and interface-isolation checks passed.
- [ ] 24-hour soak and measured resource budget available.
- [ ] Recording, manifest and reporting integrity validated.
- [ ] No hidden dependencies, network listeners or undocumented control paths.
- [ ] Build, installation and rollback reproduced by another engineer.
- [ ] Limitations, untested features and residual risks disclosed.
- [ ] Final acceptance report approved by technical reviewer.

---

# 15. Definition of done

The application is **DONE** only when the delivered system performs permitted, passive observation tasks on a real Luckfox Pico Max, produces correct and auditable recordings and offline reports, can be installed and rebuilt from source, survives the documented reliability tests, and **has no capability to issue or derive autonomous interceptor-guidance, targeting or actuator commands**.

The final agent response must include:
1. Exact features implemented.
2. Measured hardware performance, with raw evidence.
3. Requirement-to-test traceability.
4. Full PASS/FAIL/BLOCKED/NOT RUN/N/A acceptance summary.
5. Known defects, limitations and risks.
6. Build and run commands tested on the target hardware.
7. Artifact paths for source, packages, datasets and reports.

**Do not use marketing labels such as “100% accuracy,” “world-best tracking,” or “real-time AI” without a specific, reproducible measurement and a legitimate, non-weapon research definition.**
