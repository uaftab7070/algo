# Camera compatibility (HWR-002, FR-002, FR-017, AC-002, AC-006)

Observed on the Luckfox Pico Pro Max image on 2026-10-25
([device evidence](../evidence/device-20261025T090504Z/README.md)).

## Sensor identity: open (D-16)

| Check | Result |
|---|---|
| Driver bound | `mis5001` kernel module at I²C `4-0031`, sub-device `m00_b_mis5001 4-0031` |
| Driver identification message | "Detected **mis4001** 1311 sensor" |
| Other candidates | `sc3336 4-0030: Unexpected sensor id(000000)`: absent |
| Sensor output | 2592×1944 SGRBG10 at 30.000 fps (sub-device frame interval) |
| IQ tuning file used by 3A | `/etc/iqfiles/mis5001_CMK-OT2115-PC1_30IRC-F16.json` |
| Module label | **unconfirmed**: read the module/FPC label |
| Orientation | **unverified**: point the camera at a labelled test card and compare with `ahpv still` |

Until the label and orientation are confirmed, AC-002 is recorded as FAIL.

## Capture path (verified)

| Question (from the original open list) | Answer on this image |
|---|---|
| Which node is the ISP output? | `/dev/video11` = `rkisp_mainpath` (driver `rkisp_v7` 2.5.0, bus `platform:rkisp-vir0`), multiplanar API |
| Must the vendor 3A run? | **Yes** for usable images. Without it, frames arrive but are dark and green (luma ≈34). With `/oem/usr/bin/rkaiq_3A_server --silent`, exposure and white balance are correct (luma ≈141) |
| 3A lifecycle | The 3A server serves **one** stream session. A later stream with the same server instance produced no frames (`rkisp_stream_stop id:0 timeout`). Restart it before each capture run (done by `deploy/S99ahpv`) |
| Conflicts | The vendor app `rkipc` (RkLunch.sh) streams the camera at boot. Stop it first (`/oem/usr/bin/RkLunch-stop.sh`) |
| Formats | UYVY, NV16, NV61, NV21, **NV12**, NM21, NM12; 32×32 to 2592×1944, step 8. **No GREY** |
| NV12 layout | single plane, chroma at `bytesperline × height` (the adapter's assumption holds) |
| Range | quantization **full range**, recorded as `effective.color_range: FULL` and used for display |
| Timestamps | **CLOCK_MONOTONIC, end of frame** (`ts-monotonic, ts-src-eof`). `ahpv` checks the clock flag on every buffer and refuses other clocks |
| Frame rate | `G_PARM` reports no interval, so the manifest has no nominal rate. Measured delivery 25–30 fps (25.0 in the first 3A run, about 29.6 in later runs). Frame gaps are detected from driver sequence numbers |
| Latency | driver end-of-frame timestamp → `ahpv` acquisition: p50 35 ms, p99 36.5 ms |

## Effective settings (AC-006, PASS)

Manifests record the request and the driver's reply (width, height, fourcc, bytes per line,
image size, buffer count, quantization, driver/card/bus) and the measured rate. A resolution
change from 1280×720 to 640×360 was reflected exactly.

## Resolution and rate matrix (FR-017, measured on the board with the 14.6 GB vfat card)

| Mode | Data rate | Stored | Dropped (accounted) | Verdict |
|---|---|---|---|---|
| 1280×720 NV12, all frames (30 fps) | 41.5 MB/s | 7.9 fps | 74 % | outside envelope |
| 640×360 NV12, all frames (30 fps), queue 4 | 10.4 MB/s | 16.7 fps | 44 % | outside envelope |
| 640×360 NV12, keep 1 of 2, queue 16 | 5.2 MB/s | 14.1 fps | 5.9 % | marginal: write stalls up to 1.5 s |
| **640×360 NV12, keep 1 of 2, queue 32, fsync 15** | **5.2 MB/s** | **15.0 fps** | **0 % (900/900, 2700/2700, 897/897, 600/600)** | **operating envelope** |

The SD card sustains 8.5–10.5 MB/s with periodic write stalls up to 1.7 s, so the queue must
cover at least 1.7 s of frames (observed high-water 25 of 32). A faster card or hardware video
encoding could widen the envelope; re-measure after any change. A 24 h soak (AC-013) is still
needed to confirm the margin.

`pipeline.keep_every_n` is a declared transform: skipped frames receive no frame ID, are counted
in `statistics.run_frames_decimated`, appear in `source.transformations`, and do not count as gaps.
