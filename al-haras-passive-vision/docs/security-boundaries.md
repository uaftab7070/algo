# Security and safety boundaries

## Scope restriction (brief §2.2, SEC-006, NFR-012)

This software must not, and does not, convert visual observations into any command.
Concretely:

- **No control code.** There is no code path for serial ports, CAN, MAVLink, PWM, SBUS,
  CRSF, GPIO, I²C/SPI, gimbals or autopilots. `tests/acceptance/test_no_control_path.py`
  fails the build if such APIs or vocabulary appear in code (comments and strings excluded).
  The test includes a self-check that the audit detects planted identifiers.
- **No outbound network.** The binary imports no `connect`, `sendto`, `sendmsg` or
  name-resolution symbols (checked with `nm -u`). The only socket is the optional preview
  listener.
- **No tracking or targeting.** No detection, lock-on, follow, re-identification,
  prediction or aim-point computation exists. Manual annotations have a closed field
  set and cannot carry offsets or commands.
- **No configuration key** names any control interface (unit-tested).
- **Labelled outputs.** Manifests, reports, CSVs, API responses, image headers, the
  UI banner and CLI output carry `NOT FOR FLIGHT OR ENGAGEMENT CONTROL`.

The parent folder `Tracking Algo/` contains an unrelated tracking prototype. It is
excluded from this project's build and release package and must not be installed
on the same device image.

## Network listeners (SEC-003)

| Listener | Default | Bind | Purpose |
|---|---|---|---|
| Preview HTTP (TCP) | **disabled** (`preview.enabled: false`) | `127.0.0.1:8640`; a non-loopback IPv4 address requires `preview.allow_non_loopback: true` | read-only preview, replay, annotation |

To view from a workstation, keep the loopback bind and use SSH port forwarding
(if the installed image provides SSH; verify). `ahpv diag` lists the declared
listeners and, on Linux, every listening socket on the system.

The HTTP server is not TLS-protected; loopback-only binding is the control. Requests
must address the listener literally (`Host` check against DNS rebinding); cross-origin
requests are refused; CSP `default-src 'none'`; no inline script; UI text is inserted
with `textContent`.

## Roles and permissions (SEC-001, FR-015, AC-019)

| Role | View | Annotate | Review | Record (run/recover) | Configure |
|---|:-:|:-:|:-:|:-:|:-:|
| viewer | ✓ | | | | |
| annotator | ✓ | ✓ | | | |
| reviewer | ✓ | | ✓ | | |
| operator | ✓ | | | ✓ | ✓ |

- Tokens are 256-bit random values. Only their SHA-256 digests are stored, in
  `security.auth_file`, which must not be group- or world-accessible (enforced). The
  token is shown once by `ahpv auth add`.
- **Who may run `ahpv auth add` is decided by the operating system**: whoever can
  write the auth file. On the board, make the file owned by an administrative account
  and run the service as an unprivileged `ahpv` user that can read it.
- Configuration changes go only through `ahpv config set` with an operator token. The
  previous file is kept as `<config>.bak-<UTC>`, and the change is audited. The
  running service reads configuration only at start. HTTP has no configuration
  endpoint: requests to `/api/config`, `/api/control` and `/api/capture` are refused
  (403) and audited.
- Read-only CLI commands (`verify`, `report`, `still`, `sessions`, `diag`) rely on
  filesystem permissions to the data directory.

## Integrity indicators (SEC-004, NFR-009)

| Artifact | Indicator |
|---|---|
| Recorded files | SHA-256 per file in `checksums.json` and the manifest |
| Frame payloads | CRC-32 per frame (accidental corruption only, not a security control) |
| Annotation log | previous-line SHA-256 chain + per-record seal; writes are refused if the check fails |
| Audit log | previous-line SHA-256 chain + per-record seal (`ahpv audit-verify`) |
| Report bundles | SHA-256 per file |

These indicators detect accidental corruption and naive edits. They are **not
signatures**: someone with write access who recomputes the digests can forge them,
and truncating the most recent audit records is undetectable without an external
copy of the record count. Use read-only backups or off-board copies when stronger
evidence is required (see known limitations).

## Untrusted input (SEC-005)

Annotation notes, reasons and comments are length-limited (500 bytes), may not
contain control characters, are stored as inert strings, rendered as text and
neutralised in CSV. Request bodies are limited to 64 KiB, headers to 16 KiB, and the
JSON nesting depth to 64. Session and annotation IDs are validated before any path
is built. Y4M headers are parsed strictly (dimensions ≤ 8192, unknown parameters
rejected).

## Secrets (SEC-002)

No credentials exist in source, configuration examples or the package;
`test_sec002_no_embedded_credentials` scans the tree. Test tokens are generated at
test time in temporary directories.

## Retention and permissions (SEC-008)

- Sessions are never deleted by the application. Retention is an operator decision.
  The storage guard stops new writes at `storage.reserve_free_bytes` so existing
  sessions are protected.
- Recommended board policy (to be signed off by the stakeholders): keep acceptance-partition
  sessions until the final acceptance report is approved, then archive them off-board with
  their `checksums.json`. Delete development sessions only after export, using an
  administrative account, and record the deletion in the lab log.
- File modes: sessions 0750 directories / 0640 files; auth file 0600; tokens 0600.
