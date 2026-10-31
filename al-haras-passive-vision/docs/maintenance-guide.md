# Maintenance guide

## Regression testing after any change

```sh
scripts/test-report.sh
```

This runs every host suite (normal and ASan/UBSan). Commit the resulting
`evidence/host-<UTC>/summary.json` with the change. On the board, run
`bin/ahpv-unit-tests` and `scripts/smoke-test.sh` from the release package.
Repeat the hardware acceptance tests (brief §7) after any material change:
camera mode, SDK/image, compiler, storage medium or configuration defaults.

## Session housekeeping

- `ahpv sessions --config FILE` lists sessions and status. Anything still in `recording`
  while no service runs needs `ahpv recover` (operator token). `run` also does this at
  start.
- `interrupted` sessions remain valid evidence up to their last indexed frame; any
  trailing unindexed bytes are reported and never displayed.
- Back up a session by copying its whole directory. Check the copy with
  `python3 tools/verify_artifacts.py session <copy>`.
- The application never deletes data. Follow the retention policy in
  [security boundaries](security-boundaries.md#retention-and-permissions-sec-008).

## Storage

`storage.warn_free_bytes` produces a `storage_low` event. At
`storage.reserve_free_bytes` recording stops (`stopped_storage`) and the session is
finalized; preview continues. If the final manifest itself cannot be written (the device
is truly full), free space and run `ahpv recover`. Tested on the host with a 3 MB disk
image: the earlier session stayed valid.

## Power-loss test (AC-021)

Not yet performed. Procedure for the board:

1. Start a recording with `recording.fsync_interval_frames` at its deployed value.
2. After a noted time, remove power (relay or switch), wait 10 s, restore power.
3. After boot, the service recovers the session automatically; or run
   `ahpv recover --config ... --token-file ...`.
4. Run `ahpv verify` and `tools/verify_artifacts.py session` on every session and record
   `recovery.indexed_frames`, `tail_crc_failures` and `trailing_unindexed_bytes`.
5. Check the filesystem (for example with the fsck for the data partition's filesystem)
   and record the output. Repeat at least 20 times; pass if no earlier session is damaged
   and every interrupted session verifies.

Up to `fsync_interval_frames` frames before a power cut may be lost. Lower values cost
write bandwidth.

## Audit and annotation logs

`ahpv audit-verify` and `ahpv diag` check the audit chain. The annotation chain is
checked on every list and before every write. If a check fails, writes are refused:
restore the file from backup or keep it as evidence, and record the incident. Do not
edit these files by hand.

## Upgrading

1. Stop the service (SIGTERM finalizes the active session).
2. Keep the previous `/opt/ahpv` as `/opt/ahpv.prev` (rollback).
3. Unpack the new package, compare `SHA256SUMS`, run `bin/ahpv-unit-tests` and
   `scripts/smoke-test.sh`.
4. Run `ahpv config check` with the existing configuration, then start the service.
5. To roll back: stop, restore `/opt/ahpv.prev`, start. Data formats carry
   `schema_version`; a newer reader must accept older `1.0` sessions.

## Rebuilding the UI

The browser UI lives in `src/preview/ui/`. `make` regenerates `src/preview/ui_assets.cpp`
(through `scripts/embed_ui.sh`) when those files change.
