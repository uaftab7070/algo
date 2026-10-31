# SDK, toolchain and runtime compatibility

## Development host (verified)

| Item | Value |
|---|---|
| OS | macOS 13.7.8 (Darwin 22.6.0, x86_64) |
| Compiler | Apple clang 14.0.3, `-std=c++17 -Wall -Wextra -Wpedantic -Werror -Wshadow` |
| Build | GNU make (`Makefile`); CMake is **not installed**, so `CMakeLists.txt` is untested |
| Tests | Python 3.9.6 standard library; Node 24 (UI syntax check only) |
| Sanitizers | AddressSanitizer + UndefinedBehaviorSanitizer build (`make sanitize`) passes all suites |

## Target toolchain (verified on the board 2026-10-25)

| Item | Status |
|---|---|
| Board C library | uClibc-ng 1.0.31 (`/lib/libuClibc-1.0.31.so`). The vendor SDK toolchain (believed `arm-rockchip830-linux-uclibcgnueabihf`) runs only on Linux hosts and was **not used** |
| Toolchain used | **Zig 0.13.0** (`zig-macos-x86_64-0.13.0.tar.xz`, 48,857,012 bytes, SHA-256 `8b06ed10…773effd` matching ziglang.org's published index), unpacked in the session scratchpad |
| Target | `arm-linux-musleabihf`, `-mcpu=cortex_a7`, **static** (`-static`): no dependency on the board's uClibc or libstdc++ |
| Build command | `CXX="zig c++ -target arm-linux-musleabihf -mcpu=cortex_a7" AR="zig ar" LDFLAGS=-static OBJCOPY="zig objcopy" TARGET_OS=Linux TARGET_ARCH=armv7 BUILD=build/rv1106 scripts/package.sh` |
| Result | builds with `-Werror`; V4L2 adapter compiled for the first time; stripped `ahpv` 1.2 MB, `ahpv-unit-tests` 1.1 MB (ELF 32-bit ARM EABI5, statically linked) |
| On-board tests | 31/31 unit tests, smoke test, live V4L2 capture, recovery, offline, authorization; all PASS |
| Portability fix needed | `__int128` (unsupported on 32-bit ARM) replaced with overflow-safe 64-bit arithmetic in the Y4M timestamp code |
| Package install on BusyBox | `tar` has no `-z`; use `gunzip -c pkg.tar.gz \| tar -x`. `sha256sum -c SHA256SUMS` works |
| Vendor-SDK build path | `CMakeLists.txt` remains untested (no CMake/SDK host); the Makefile + Zig path is the verified one |

## Vendor media and NPU libraries

Not linked. The only vendor component used at run time is the separate `rkaiq_3A_server` process (exposure/white balance), started next to `ahpv`. Hardware video encoding (VENC), RGA scaling and RKNN are not integrated,
because their APIs, versions and the installed runtime have not been verified (brief
§5). Library names that appear in `inventory.sh` output do not establish ABI
compatibility.

## Optional AI runtime assessment (brief §6)

**Not applicable for this release (AC-010: NOT APPLICABLE).** AI inference is not needed
for the passive application, and the brief advises against adding it for appearance.

| Step | Status |
|---|---|
| Toolchain (RKNN-Toolkit2) | NOT TESTED |
| Conversion | NOT TESTED |
| Load on device | NOT TESTED |
| Inference | NOT TESTED |
| Memory usage | NOT TESTED |

No performance figure is claimed. If a benign compatibility study is later approved,
it must run as a separate tool that publishes no target detection, lock or follow
output.
