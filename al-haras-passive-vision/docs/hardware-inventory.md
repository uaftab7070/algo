# Hardware inventory (HWR-001 … HWR-005)

Observed on the physical unit on 2026-10-25 over USB-C/ADB. Raw data:
[`evidence/device-20261025T090504Z/inventory.txt`](../evidence/device-20261025T090504Z/inventory.txt)
(collected by the read-only `scripts/inventory.sh`) and the device evidence README.

## HWR-001 Board identity

| Item | Observed |
|---|---|
| SKU (device tree model) | Luckfox Pico **Pro** Max |
| SoC | Rockchip RV1106G3 (`rockchip,rv1103g-38x38-ipc-v10`, `rockchip,rv1106g3`) |
| CPU | 1× ARMv7 Cortex-A7 (0xc07 r0p5), NEON, VFPv4, idiv |
| Board serial | `f27110228fd4e70f` (also the USB/ADB serial) |
| Bootloader | U-Boot, `androidboot.fwver=uboot-06/26/2025` |
| Kernel | Linux 5.10.160 #22, built 2025-06-11, armv7l |
| Userspace | Buildroot 2023.02.6 (`-g2ae728b52-dirty`), uClibc-ng 1.0.31, BusyBox 1.36.1, Python 3.11.6 |
| Partition map (SPI NAND, `mtdparts`) | env 256K, idblock 256K, uboot 512K, boot 4M, oem 30M, userdata 10M, rootfs 210M |
| Filesystems | `/` UBIFS (137 MB free), `/oem` UBIFS (20 MB, 81 % used), `/userdata` UBIFS (2.2 MB), `/mnt/sdcard` vfat 14.6 GB |
| Board revision | not visible in software; read the silkscreen |
| SDK version | not used. The application was built with Zig 0.13.0 (static musl), independent of the vendor SDK |

## HWR-002 Camera

See [camera compatibility](camera-compatibility.md). The camera enumerates and captures.
Its identity is open: the driver reports MIS4001 and the module label is unconfirmed (D-16).

## HWR-003 Resource budget (measured, awaiting sign-off)

| State | MemAvailable | System CPU | ahpv RSS (peak) | SoC temp | Source |
|---|---|---|---|---|---|
| After boot, vendor app running | 158,076 kB (MemTotal 185,564 kB) | — | — | 57.7 °C | inventory |
| Idle, vendor app stopped | ≥158.7 MB | 7.4 % mean, 9.3 % max | — | ≤48.2 °C | `resource-profile-idle.csv` |
| Camera streaming + recording + live preview (envelope mode) | ≥147.3 MB | 30.9 % mean, 42 % max | 10.6 MB (HWM 12.3 MB) | ≤55.9 °C | `resource-profile-recording.csv` |
| Recording only (envelope mode, 60 s) | ≥150.1 MB | 20.8 % mean | 12.2 MB | ≤53.5 °C | envelope session metrics |

- CMA: 66 MB (`rk_dma_heap_cma=66M`), CmaFree 0 kB in every state (fully reserved by the vendor
  DMA heap; V4L2 buffers come from it).
- Budget proposal: `ahpv` ≤ 16 MB RSS (queue 32 × 0.35 MB + about 5 MB); about 130 MB headroom remains.
  **Needs stakeholder signature (AC-014).**

## HWR-004 Interfaces

| Interface | Observed | Used by ahpv |
|---|---|---|
| MIPI CSI-2 | csi2-dphy0, data rate 891 Mbps, MIS5001 driver at I²C 4-0031 | yes (camera) |
| USB-C (device) | ADB + RNDIS `usb0` 172.32.0.93/16 | no (ADB used for bring-up only) |
| Ethernet | `eth0` present, no address during tests | no |
| microSD | `mmcblk1p1` vfat 14.6 GB | recordings |
| Console UART | `/dev/ttyFIQ0` (getty) | **never opened** |
| NPU / RGA / MPP | `/dev/rknpu`, `/dev/rga`, `/dev/mpp_service` | **not used** |
| Thermal | `soc-thermal` | read for metrics |
| Network listeners (vendor image) | telnetd 0.0.0.0:23, sshd 0.0.0.0:22, adbd 0.0.0.0:5555, rkipc RTSP 0.0.0.0:554, ntpd UDP 123, adb 127.0.0.1:5037, 127.0.0.1:3893 | ahpv adds only 127.0.0.1:8640 when preview is enabled |

```text
 [MIPI camera, mis5001 driver] --CSI-2--> [RV1106 ISP rkisp] --/dev/video11 V4L2--> ahpv --> /mnt/sdcard/ahpv (vfat)
          ^ rkaiq_3A_server (vendor 3A, per capture run)                               \--> 127.0.0.1:8640 (optional)
                                                                                              --adb forward / ssh -L--> browser
 Console UART: getty only. ahpv has no connection to any flight controller, receiver, gimbal or actuator.
```

## HWR-005 Power and storage

| Item | Status |
|---|---|
| Power | USB-C from the development host (500 mA requested descriptor). Measured current: not instrumented |
| SD write throughput | 8.5–10.5 MB/s sustained (64 MB `dd conv=fsync`, two runs) |
| SD filesystem health | vfat reports `Invalid FSINFO signature`. Run `fsck.vfat` (present at `/usr/sbin/fsck.vfat`) or reformat before acceptance recordings |
| Process-kill recovery | PASS on the board (AC-017) |
| Power-loss recovery | **BLOCKED**: needs a physical power cut ([maintenance guide](maintenance-guide.md#power-loss-test-ac-021)) |
| Clock | RTC present (`rockchip-rtc`) but unset (2021-01-01); ntpd runs without a network. Set it before dated acceptance recordings |
