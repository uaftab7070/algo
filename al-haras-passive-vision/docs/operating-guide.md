# Operating guide

NOT FOR FLIGHT OR ENGAGEMENT CONTROL. Every command below works offline.

## 1. Build

Host (verified on macOS): `scripts/build.sh` → `build/host/ahpv`.
Board (not yet performed): see [SDK compatibility](sdk-compatibility.md).

## 2. Configure

Start from `config/example-config.yaml` (board) or `config/host-replay.yaml` (host).
Validate a file without touching the system:

```sh
ahpv config check --config config.yaml
```

Errors name the line and the allowed range, for example
`line 12: pipeline.queue_capacity must be between 1 and 64, got 0`.

## 3. Create principals

```sh
ahpv auth add --config config.yaml --principal lab-operator --role operator
```

Roles: `viewer`, `annotator`, `reviewer`, `operator`. The token is printed once. Store it
in a file only its user can read (`chmod 600`) and pass it with `--token-file`, or set it
in `AHPV_TOKEN`.

## 4. Record

```sh
ahpv run --config config.yaml --token-file operator.token --purpose "bench scene 3, static objects" --partition development --owner "Lab A" --permission-record "LAB-2026-014" --place-category "indoor lab" --scene-label "static objects"
```

- `--partition` is `development` or `acceptance` (DAT-002). Keep acceptance footage
  separate from anything used to tune thresholds.
- `--purpose` is required; the other provenance flags default to `unspecified`, and the
  QA report then shows `provenance_complete: false`.
- `--duration-s N`, `--max-frames N` limit a run; `--exit-on-end` exits when input ends
  even if the preview is enabled.
- `--inventory inventory.json` links the board inventory to every manifest.
- Stop with Ctrl-C or SIGTERM: the session is finalized with checksums.
- On start, sessions left in `recording` state by a crash or power loss are recovered first
  (status `interrupted`), and the result is printed as `startup_recovery`.

Each run prints a JSON summary with its session IDs. Long runs roll over every
`recording.segment_max_frames` frames into linked sessions (`run_id`, `segment_index`,
`previous_session_id`).

### Exit statuses

| Code | Meaning |
|---|---|
| 0 | completed (end of input, limit reached or operator stop) |
| 1 | a check failed (`verify`, `audit-verify`, rejected annotation) |
| 2 | usage or configuration error |
| 3 | camera/source absent at start, or lost during capture without successful reopen |
| 4 | unrecoverable source error (malformed or truncated input) |
| 5 | internal error, including memory allocation failure |
| 6 | unauthorized (missing/invalid token, insufficient role, unsafe auth file) |

## 5. Preview and review

Enable `preview.enabled: true`, run `ahpv run` (live) or `ahpv serve` (recorded sessions
only), then open `http://127.0.0.1:8640/` and paste a token.

- **Live preview** shows the newest frame with its ID, acquisition time, age, flags and
  display transform. If no frame arrived within `source.no_signal_timeout_ms`, or the
  camera is lost, the image is cleared and replaced by `NO CURRENT FRAME — <state>`. A
  stale image is never shown as live.
- **Recorded sessions**: select a session, then use |< < Play > >| and the seek bar.
  Replay steps through stored frames; it does not reproduce real-time pacing. Dropped or
  rejected frame IDs are never displayed. A frame that fails its CRC is refused (HTTP 409).
- **Annotate** (annotator role): drag a rectangle, type notes, **Save new**. Select an
  annotation to **Update selected** (redraw and/or edit notes) or **Delete** (with a
  reason; the original stays in history). **History** shows every revision.
- **Review** (reviewer role): select, choose accepted / needs_revision / rejected, add a
  comment. You cannot review your own annotations when independent review is required.

To view the board's preview from a workstation, keep the loopback bind and tunnel:

```sh
ssh -L 8640:127.0.0.1:8640 <user>@<board-address>
```

(SSH availability on the installed image is unverified.)

## 6. Annotate from the command line

```sh
ahpv annotate add --config config.yaml --token-file annotator.token SESSION --frame 42 --region 20,30,100,75 --notes "authorized laboratory object"
```

Also: `annotate list|history|revise|delete|review` (see `ahpv` usage). Coordinates are
source pixels with the origin at top left. Out-of-bounds regions are rejected and the
error states the valid bounds.

## 7. Verify, report and export

```sh
ahpv verify --config config.yaml SESSION
```

```sh
ahpv report --config config.yaml SESSION --contact-sheet 9
```

```sh
python3 tools/verify_artifacts.py session <data_root>/sessions/SESSION
```

`report` writes a new `reports/<report_id>/` with `qa-report.json`, `summary.csv`,
`frames.csv`, `annotations.csv`, a contact sheet and checksums. `still` exports one frame
as BMP; `contact-sheet` exports a sheet on its own.

## 8. Diagnostics

- `ahpv diag --config config.yaml`: software build and hash, platform, current
  resources, sessions by status, declared and observed listeners, auth-file and
  audit-chain status.
- `ahpv inventory`: read-only platform inventory (JSON).
- `ahpv audit-verify --config config.yaml`: checks the audit hash chain and seals.
- The structured log (`logging.file`) uses the event IDs in [data schemas](data-schemas.md).

## 9. Change configuration

```sh
ahpv config set --config config.yaml --token-file operator.token preview.port 8641
```

This needs an operator token. The old file is kept as `config.yaml.bak-<UTC>`, the change
is audited, and it takes effect at the next service start.

## 10. On the Luckfox Pico Pro Max (verified 2026-10-25)

1. Install from the release package (BusyBox `tar` has no `-z`):

   ```sh
   gunzip -c ahpv-0.1.0-linux-armv7.tar.gz | tar -x && cd ahpv-0.1.0-linux-armv7 && sha256sum -c SHA256SUMS
   ```

2. Keep `config.yaml`, `auth.conf`, tokens and the audit log on UBIFS (`/etc/ahpv` or
   `/root/ahpv`). vfat on the SD card cannot hold owner-only permissions, so use it for
   recordings and the log only.
3. The vendor app `rkipc` holds the camera: stop it with `/oem/usr/bin/RkLunch-stop.sh`
   (this also stops the Ethernet DHCP client).
4. Start the vendor 3A service **fresh for each capture run**:
   `/oem/usr/bin/rkaiq_3A_server --silent &`, wait about 1 s, then `ahpv run …`; stop it afterwards.
5. Use the measured operating envelope in `config/example-config.yaml` (640×360 NV12,
   `keep_every_n: 2`, queue 32, fsync 15). Larger modes overflow this SD card; drops are
   accounted, but footage is lost.
6. To view the preview from a workstation over USB: `adb forward tcp:8640 tcp:8640`, then open
   `http://127.0.0.1:8640/`. The board listener stays on its own loopback.

## 11. Start at boot (FR-001, not yet exercised as a boot script)

Install `deploy/S99ahpv` as `/etc/init.d/S99ahpv` with the layout described in the script.
`APP-001` in the log (with the kernel `boot_id`) marks readiness;
`tests/hardware/boot_cycle_check.sh` records it once per boot for AC-001.
