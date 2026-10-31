# Data schemas, file layout and event catalogue

Schema version `1.0`. Machine-readable schemas are in [`schemas/`](../schemas/); the
independent checker is `tools/verify_artifacts.py`. Every observation output
carries `usage_notice: "NOT FOR FLIGHT OR ENGAGEMENT CONTROL"`.

## Session directory

`<data_root>/sessions/<session_id>/`, where `session_id` = `s-YYYYMMDDTHHMMSSZ-<8 hex>`
(created with an exclusive `mkdir`, so IDs cannot collide).

| File | Written | Content | Schema |
|---|---|---|---|
| `session.json` | atomically (temp file, fsync, rename, directory fsync) at start and at finalize | manifest: software, platform, clocks, source (requested and effective), configuration, pipeline policy, provenance, recording parameters, status, statistics, checksums | `session-manifest.schema.json` |
| `video.y4m` | append | YUV4MPEG2 header, then `FRAME\n` + planar payload per stored frame | Y4M (8-bit mono / 4:2:0 / 4:4:4, progressive) |
| `frames.jsonl` | append, after its payload | one record per stored frame | `frame-record.schema.json` |
| `events.jsonl` | append | drops, rejections, signal, storage and lifecycle events | below |
| `metrics.jsonl` | append, every `metrics.sample_interval_ms` | resource sample, queue depth, counters | below |
| `checksums.json` | at finalize/recovery | SHA-256 and size of the four files above | — |
| `annotations.jsonl` | append (locked) | annotation event log, hash-chained and sealed | `annotation.schema.json` (the `annotation` member) |
| `reports/<report_id>/` | per `ahpv report`, never overwritten | `qa-report.json`, `summary.csv`, `frames.csv`, `annotations.csv`, `contact-sheet.bmp/.json`, `checksums.json` | `qa-report.schema.json`, `csv-columns.json` |

### Session status values

`recording` (active, or interrupted before recovery) · `complete` · `segment_complete`
(rolled over to a linked next segment) · `camera_lost` · `source_error` · `stopped_storage` ·
`error` · `interrupted` (recovered after abrupt termination).

## Timestamps (AC-023)

| Field | Clock | Meaning |
|---|---|---|
| `acquisition_mono_ns` | CLOCK_MONOTONIC | when this process obtained the frame; used for ordering and intervals; strictly increasing |
| `acquisition_wall_ns` | CLOCK_REALTIME | the same moment as calendar time; may step (NTP/RTC). Steps are reported by `ahpv verify` as warnings |
| `source_timestamp_ns` | per `source.timestamp_basis` | `file_nominal_frame_rate`: index × frame period; `stream_delivery_monotonic`: equals acquisition (a pipe carries no capture time); V4L2: the driver buffer timestamp (clock and exposure point **must be verified on the device**) |
| `interval_ns` | source basis | difference to the previous frame of the same source epoch |

`session.json.clocks` records both clocks at session creation, so wall time can be
related to monotonic time for that boot. Display/processing time is never written
into any of these fields; `processing_ns` is separate and non-deterministic.

## Frame flags

`corrupted` · `sequence_gap` (source counter skipped, or no counter and interval > late
factor) · `late` (interval > `validation.late_factor` × nominal) · `timestamp_regression` ·
`repeated_content` (CRC equal to previous frame) · `low_contrast` (luma std < threshold;
possible no-signal/covered lens) · `very_dark` · `very_bright` · `after_discontinuity`
(first frame after reopen) · `after_no_signal` · `fault_injected` (test fixture only).

Quality flags describe the image and never invalidate it. Corrupted frames are
rejected: they are not stored, and they appear only as `rejected_frame` events.

## Frame accounting (AC-005)

Within a session every assigned `frame_id` between the first and last appears exactly once as:
a `frames.jsonl` record (stored), a `queue_drop` event (discarded by the bounded live
queue), or a `rejected_frame` event (corrupt input). Frames the *source* never delivered
are not assigned IDs; they appear as `missing_before` on the next frame and as
`sequence_gap`/`late` flags.

## Event types (`events.jsonl`)

Every event has `event`, `at_mono_ns`, `at`.

| Event | Extra fields |
|---|---|
| `queue_drop` | `frame_id`, `source_epoch`, `source_sequence`, `acquisition_mono_ns`, `policy` |
| `rejected_frame` | `frame_id`, `source_epoch`, `source_sequence`, `reason`, `fault_injected` |
| `no_signal` / `signal_restored` | `message` / `next_frame_id` |
| `source_disconnected`, `source_reopened`, `source_reopen_failed`, `source_error` | `message`, `attempt`, `source_epoch` |
| `end_of_stream`, `operator_stop`, `capture_limit_reached` | — |
| `storage_low`, `recording_stopped` | `free_bytes` / `status`, `reason` |
| `memory_failure` | `stage` |
| `session_finalized`, `session_recovered` | `status` / recovery summary |

## Annotation record (current state)

See `schemas/annotation.schema.json`. The property set is closed: requests that contain
any other field (for example `aim_point`, `steering_offset`, `command`) are rejected with
HTTP 400. `annotator_id` is taken from the authenticated principal, not from the request
body. `source_timestamp_ns` and `timestamp_basis` are copied from the frame index.
`region_xywh_pixels` are integers in source pixels (origin top-left) and must lie
inside `image_size_pixels`.

The log (`annotations.jsonl`) holds one event per line: `create`, `revise`, `delete`
(tombstone; the original is retained) or `review`. Each event carries
`previous_event_sha256` (the hash of the previous line) and `record_sha256` (its own seal).
Revision uses optimistic concurrency (`expected_revision`). A revision resets
`review_status` to `unreviewed`. With `annotations.require_independent_review`, a
reviewer cannot review an annotation they created or last revised.

## QA report

`deterministic`: frame counts, flag and event counts, source-interval distribution,
luma statistics, unusable spans, upstream gaps, usable fraction, annotation
completeness, integrity verdict. For identical input, build and configuration these
fields are identical (verified by `test_ac007_replay_reproducibility`).
`nondeterministic`: acquisition intervals, processing time, measured effective frame
rate, resource samples. `environment` embeds recording and report platforms, software
build and executable hash, configuration hash, manifest and video hashes (DAT-006).
Distributions report count, min, mean, p50, p95, p99 and max in milliseconds from a
fixed-memory histogram (exact below 0.128 ms, at most 1.6 % relative error above).

## CSV exports

Column lists are fixed in `schemas/csv-columns.json`. Text cells that start with
`= + - @` or a tab are prefixed with `'` (spreadsheet formula neutralisation).

## Log event catalogue (structured log, JSON lines)

Each record has `ts`, `mono_ns`, `level`, `event_id`, `software_version`, `session_id`,
`message`, optional `details`.

| ID | Meaning |
|---|---|
| APP-001 | capture started (readiness marker; includes kernel `boot_id`) |
| APP-002 | capture ended, preview still available read-only |
| APP-090 | unhandled error at top level |
| CFG-001 | configuration rejected |
| CAP-001 / 002 / 003 | source initialization failed / opened / end of stream |
| CAP-010 / 011 | no signal / signal restored |
| CAP-020 | corrupted frame rejected |
| CAP-030 / 031 / 032 | source disconnected / reopened / reopen failed |
| CAP-040 | unrecoverable source error |
| PIP-002 / 010 | pipeline finished / queue overflow drops |
| REC-001 / 002 / 003 | session started / finalized / next segment failed |
| REC-010 | storage low |
| REC-020 / 021 / 022 | recording stopped / event log write failed / metrics write failed |
| REC-030 / 031 | checksum or final manifest write failed (recover later) |
| REC-040 / 041 | interrupted session recovered / could not be inspected |
| RES-010 | memory allocation failure, stopped safely |
| MET-001 | resource sample |
| ANN-001 | annotation event written |
| PRV-001 / 002 | preview listener started / stopped |
| PRV-090 / 091 | request handling failure |
| SEC-010 / 020 | audited action allowed / denied |
| SEC-090 | audit log write failed |
