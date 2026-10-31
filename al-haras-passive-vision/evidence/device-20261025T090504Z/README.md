# Device evidence: Luckfox Pico Pro Max, 2026-10-25

NOT FOR FLIGHT OR ENGAGEMENT CONTROL.

Connection: USB-C to the macOS development host, ADB (device serial `f27110228fd4e70f`).
The board clock was unset (reads 2021-01-01; no NTP, RTC not set), so board-side
wall times show 2021. Ordering uses CLOCK_MONOTONIC. Host times are 2026-10-25
(UTC+5 local, about 09:05–10:00 UTC).

## Board identity (HWR-001; from `inventory.txt`)

| Item | Observed |
|---|---|
| Model (device tree) | Luckfox Pico Pro Max (`rockchip,rv1103g-38x38-ipc-v10`, `rockchip,rv1106g3`) |
| CPU | 1× ARMv7 Cortex-A7 (part 0xc07, r0p5), NEON/VFPv4 |
| Kernel | Linux 5.10.160 #22 (2025-06-11), armv7l |
| Userspace | Buildroot 2023.02.6 (`-g2ae728b52-dirty`), uClibc-ng 1.0.31, BusyBox 1.36.1, Python 3.11.6 |
| U-Boot | `androidboot.fwver=uboot-06/26/2025` |
| Storage | SPI NAND: env 256K, idblock 256K, uboot 512K, boot 4M, oem 30M, userdata 10M, rootfs 210M (UBIFS); microSD 14.6 GB vfat at `/mnt/sdcard` |
| Memory | MemTotal 185,564 kB (of 256 MB installed); CMA 66 MB (`rk_dma_heap_cma=66M`), fully reserved by the vendor DMA heap (CmaFree 0) |
| Camera | `mis5001` driver at I²C 4-0031; driver log "Detected **mis4001** 1311 sensor" (see open item); SC3336 probe failed (absent) |
| Video path | `/dev/video11` = `rkisp_mainpath` (rkisp_v7 2.5.0, multiplanar); sensor 2592×1944 SGRBG10 at 30 fps (subdevice) |
| NPU / RGA / MPP | `/dev/rknpu`, `/dev/rga`, `/dev/mpp_service` present; **not used** |
| Thermal | `soc-thermal` zone; 48 °C idle, ≤55.9 °C while recording |
| Vendor services at boot | `rkipc` (RkLunch.sh; holds the camera; RTSP 0.0.0.0:554), sshd 0.0.0.0:22, telnetd 0.0.0.0:23, adbd TCP 0.0.0.0:5555, ntpd |

## Camera path findings (HWR-002 / AC-002 / AC-006 / AC-023)

- `rkipc` must be stopped first (`/oem/usr/bin/RkLunch-stop.sh`; also kills `udhcpc`).
- `/dev/video11` formats: UYVY, NV16, NV61, NV21, NV12, NM21, NM12 (no GREY), 32×32–2592×1944, step 8.
- NV12 is single-plane with chroma at `bytesperline × height`; quantization is **full range**.
- Buffer timestamps are **CLOCK_MONOTONIC, end of frame** (`ts-monotonic, ts-src-eof`).
- `G_PARM` reports no frame interval. Delivered rate measured 25.00 fps (first `v4l2-ctl`
  run with 3A) and 29.6–30 fps in later `ahpv` runs.
- Without the vendor 3A the image is dark and green (`camera-frame-1280x720-no3A.png`, luma 34).
  With `/oem/usr/bin/rkaiq_3A_server --silent` it is correctly exposed
  (`camera-frame-1280x720-with3A.png`, luma 141; IQ file `mis5001_CMK-OT2115-PC1_30IRC-F16.json`).
- The 3A server serves **one** stream session. A second stream started while it ran
  produced no frames and `rkisp_stream_stop id:0 timeout`. Restart it before each capture
  run (done in `deploy/S99ahpv`).
- Orientation was **not verified** (no labelled test card).

## Storage (HWR-005 / D-01)

64 MB synced writes to the SD card took 6.08 s and 7.52 s → **8.5–10.5 MB/s sustained**.
The FAT filesystem reports `Invalid FSINFO signature` (harmless; run `fsck.vfat` and reformat
before acceptance recordings). FAT cannot hold owner-only permissions, so `auth.conf`,
tokens and the audit log live in `/root/ahpv` (UBIFS).

## Builds used on the board

Static armv7 binaries from Zig 0.13.0 (`arm-linux-musleabihf`, `-mcpu=cortex_a7`), SHA-256
of the Zig tarball checked against ziglang.org's index. Each session manifest records its build:

| Build ID | Change from previous |
|---|---|
| `97e3ebfe6fd49b54` | first cross build (V4L2 timestamp check, colour range, 32-bit-safe Y4M math) |
| `568e11c75e85aca9` | + `pipeline.keep_every_n` decimation |
| `ac9bf50161320919` | + live preview colour range and latency fixes |
| `3d32c6249a1f5bd9` | + build-stamp/Makefile fixes; **release package**, installed and tested (AC-018) |

## Sessions recorded on the board (`/mnt/sdcard/ahpv/sessions`)

| Session | Mode | Assigned / stored / dropped | Stored fps | Purpose / result |
|---|---|---|---|---|
| s-20210101T121714Z-1b1656c8 | 1280×720 | 0 / 0 / 0 | — | 3A server wedged → no frames; `no_signal` reported, nothing fabricated |
| s-20210101T121910Z-29bf9b6e | 1280×720, no 3A | 300 / 79 / 221 | 7.9 | SD bandwidth overload; every drop accounted (`session-720p-no3A/`) |
| s-20210101T122024Z-0945f6a2 | 640×360, 3A | 599 / 334 / 265 | 16.7 | full 30 fps exceeds SD rate (`session-360p-3A/`) |
| s-20210101T122434Z-c7d02f23 | 640×360, keep 1/2, queue 16 | 900 / 847 / 53 | 14.1 | write stalls up to 1.5 s (`session-envelope-try1/`) |
| **s-20210101T123413Z-640c36a8** | **640×360, keep 1/2, queue 32** | **900 / 900 / 0** | **15.0** | **operating envelope** (`session-envelope-640x360-15fps/`) |
| s-20210101T123804Z-383c7bca | envelope + preview | 2700 / 2700 / 0 | 15.0 | 3-minute live preview run (FR-004) |
| s-20210101T124524Z-a8bb0e09 | envelope + preview | 897 / 897 / 0 | 15.0 | preview recheck after fixes: age 76–160 ms, full range |
| s-20210101T124643Z-1719b47e | envelope | interrupted (165 indexed) | — | AC-017: SIGKILL → recover → verifies |
| s-20210101T124750Z-b85059d0 | envelope | 300 / 300 / 0 | 15.0 | AC-016: all network interfaces down |
| s-20210101T124925Z-4807d605 | envelope + preview | 600 / 600 / 0 | 15.0 | AC-014 resource profile |

Raw video stays on the SD card (311 MB per minute in the envelope mode). This folder holds the
manifests, frame indexes, events, metrics, reports, contact sheets and verification outputs
pulled for the key sessions.

## Measurements (envelope session s-20210101T123413Z-640c36a8, build 568e11c75e85aca9)

| Metric | Value |
|---|---|
| Stored rate | 15.006 fps (900/900, 899 decimated by configuration) |
| Stored-frame interval | p50 67.6 ms, p95 68.6 ms, max 75.5 ms |
| Processing (validate + record) | p50 5.2 ms, p95 266 ms, p99 1.70 s (SD write stalls) |
| Queue high-water | 25 of 32 |
| Driver timestamp → acquisition | p50 35.1 ms, p99 36.5 ms, max 58.6 ms |
| Process CPU | mean 14.8 %, max 27.4 %; system mean 20.8 % |
| RSS | peak 12.2 MB |
| MemAvailable during run | ≥ 150 MB |
| Live preview (after fix) | frame age at serve 76–160 ms, BT.601 full range |

Resource profiles: `resource-profile-idle.csv` (idle: MemAvailable ≥158.7 MB, system CPU 7.4 %,
48.2 °C) and `resource-profile-recording.csv` (recording + preview: MemAvailable ≥147.3 MB,
system CPU 30.9 % mean / 42 % max, ahpv RSS ≤10.6 MB, HWM 12.3 MB, ≤55.9 °C).

## Interface audit while capturing (`device-audit-while-running.txt`)

`ahpv` held only `/dev/video11`, its session files, its log and two sockets (the
`127.0.0.1:8640` listener and one forwarded browser connection). No serial, I²C, SPI, GPIO, CAN
or PWM handles. It ran as root during bring-up; deployment must use an unprivileged user.

## Changes made on the board

- `rkipc` stopped with the vendor stop script (runtime only; it returns at the next boot).
- `/root/ahpv/` created: configs, `auth.conf` (0600), tokens (0600), `audit.jsonl`, `ahpv.log`.
- `/mnt/sdcard/ahpv/` created with the sessions above.
- Binaries and test copies only in `/tmp` (cleared at reboot).
- Network interfaces were taken down for AC-016 and restored to their previous state.
- No boot scripts, vendor services, firmware or security settings were changed.
