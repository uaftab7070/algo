# Releases

`scripts/package.sh` writes `ahpv-<version>-<os>-<arch>.tar.gz` plus a `.sha256` file here.
Each package contains `bin/`, `config/`, `schemas/`, `docs/`, `scripts/` (smoke test,
inventory), `tools/verify_artifacts.py`, `deploy/S99ahpv`, `tests/hardware/`, a fixture,
`BUILD-INFO.txt` and `SHA256SUMS`.

| Package | Target | Built on | Hardware verified |
|---|---|---|---|
| ahpv-0.1.0-darwin-x86_64 | macOS host (review/replay workstation) | macOS 13.7.8, Apple clang 14.0.3 | n/a (host) |
| ahpv-0.1.0-linux-armv7 | Luckfox Pico Pro Max (static musl, Zig 0.13.0) | macOS 13.7.8 | **yes**: installed from this tarball on 2026-10-25; checksums, 31 unit tests and smoke test pass on the board |

Install on the board: `gunzip -c ahpv-0.1.0-linux-armv7.tar.gz | tar -x` (BusyBox `tar` has
no `-z`), then `sha256sum -c SHA256SUMS`, `bin/ahpv-unit-tests`, `scripts/smoke-test.sh`, and
follow `docs/operating-guide.md` §10. Rollback: see `docs/maintenance-guide.md`.

NOT FOR FLIGHT OR ENGAGEMENT CONTROL.
