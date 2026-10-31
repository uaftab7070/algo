# Architecture

NOT FOR FLIGHT OR ENGAGEMENT CONTROL. Observation, recording and review only.

## Data flow

```text
 Camera (V4L2, UNVERIFIED)   Live Y4M pipe/FIFO   Y4M file (replay)
            \                       |                    /
             +-------- capture/  FrameSource ---------+        (optional test-only FaultInjectingSource)
                                    |
             capture thread: frame_id, source_epoch, SequenceTimingAnalyzer
             (gaps / late / regressions / no-signal / disconnect / reopen events)
                                    |
                  frames/ BoundedQueue  (live: drop_oldest|drop_newest, every
                                    |    drop logged by frame ID; files: lossless backpressure)
             consumer thread: ContentAnalyzer (CRC-32, luma stats, quality flags)
                    |                   |                         |
          recording/ Recorder     latest-frame slot        metrics/ histograms +
          video.y4m, frames.jsonl, (read-only preview)     ResourceSampler -> metrics.jsonl
          events.jsonl, checksums        |
                    |             preview/ HTTP (127.0.0.1, token auth, CSP)
          annotations/ (append-only,     |  GET frames/status/sessions
           hash-chained, sealed)  <------+  POST/PUT annotation records only
                    |
          metrics/ report -> qa-report.json, summary.csv, frames.csv, annotations.csv, contact sheet

          [HARD ISOLATION: no serial, CAN, MAVLink, PWM, SBUS, gimbal, outbound network or actuator code]
```

## Modules (`src/`)

| Module | Responsibility | Key files |
|---|---|---|
| `common/` | JSON (strict, int64-exact), SHA-256, CRC-32, clocks, durable file I/O, structured log, record seals | `json.cpp`, `hash.cpp`, `fsutil.cpp`, `log.cpp`, `seal.hpp` |
| `platform/` | Executable identity, read-only inventory, listener enumeration | `platform.cpp` |
| `capture/` | Source interface, Y4M file/stream reader, V4L2 adapter (Linux only), fault-injection fixture | `y4m_source.cpp`, `v4l2_source.cpp`, `fault_injection.cpp` |
| `frames/` | Frame model and flags, timing/content analysis, bounded queue, pipeline threads | `frame.cpp`, `bounded_queue.hpp`, `pipeline.cpp` |
| `recording/` | Session store, recorder + storage guard, loader, verifier, recovery | `session.cpp`, `recorder.cpp`, `verify.cpp` |
| `annotations/` | Manual ROI records, revisions, review, history | `annotations.cpp` |
| `preview/` | Loopback HTTP server, API routes, embedded UI | `http_server.cpp`, `preview_service.cpp`, `ui/` |
| `metrics/` | Histograms, resource sampling, image export, QA reports | `histogram.cpp`, `resources.cpp`, `images.cpp`, `report.cpp` |
| `security/` | Configuration parsing/validation, roles and tokens, audit log | `config.cpp`, `auth.cpp`, `audit.cpp` |
| `app/` | `ahpv` command-line interface | `main.cpp` |

## Design decisions (ADR summary)

1. **Native C++17, no third-party runtime dependencies.** The board has a single
   Cortex-A7 core and 256 MB RAM; the binary is about 1 MB on the host and needs
   only libc/libstdc++/pthreads. No neural inference (brief §1.4, §6).
2. **Raw planar Y4M recording.** It needs no encoder, is byte-reproducible, and
   allows random-access, per-frame CRC verification. The cost is storage bandwidth
   (see known limitations). Hardware encoding (RKMPI VENC) is deliberately not
   faked, because the SDK has not been verified.
3. **Two clock domains, never mixed.** `acquisition_mono_ns` (CLOCK_MONOTONIC) orders
   frames and measures intervals; `acquisition_wall_ns` dates them;
   `source_timestamp_ns` follows the declared `timestamp_basis`.
4. **Bounded memory.** Fixed-capacity queue; fixed-size histograms; session
   segmentation (`recording.segment_max_frames`) bounds index size for review.
5. **No silent loss.** Every assigned frame ID ends up stored, `queue_drop` or
   `rejected_frame`. The verifier and the independent verifier both check this.
6. **Append-only evidence.** Recordings are never rewritten. Recovery adds records
   instead of truncating. Annotation and audit logs are hash-chained, and every
   record is sealed.
7. **Least exposure.** The preview listener is off by default and binds only an
   IPv4 literal (non-loopback requires an explicit flag). HTTP can write annotation
   records only. Configuration changes need the CLI, an operator token and file access.
8. **Explicit failure states.** No signal, camera lost, source error, storage stop
   and memory failure are distinct states. The preview returns 503 rather than a
   stale image.

## Threads

| Thread | Work | Blocking behaviour |
|---|---|---|
| capture | `FrameSource::read`, ID assignment, timing analysis, queue push | live: never blocks on the queue; files: backpressure |
| consumer (main) | validation, recording, latest-frame publication, metrics | bounded by the 100 ms queue wait |
| preview | accept + one request at a time (5 s socket timeouts) | never touches capture state except through copies |

Shutdown: SIGINT/SIGTERM stop capture, drain the queue, finalize the session
(checksums, manifest) and stop the listener. Exit statuses are listed in the
[operating guide](operating-guide.md).
