# Luckfox Pico Max / camera / DakeFPV H743 integration

Updated 2026-10-24. This is the hardware-specific implementation plan, not a
claim that the existing application has been deployed on these boards.

## Confirmed by the user

- Companion computer: Luckfox Pico Max.
- Camera: supplied as “MSI 5001”; confirm the module label is **MIS5001**.
- Flight controller: DakeFPV H743, running Betaflight (user wrote “Betafly”);
  exact board revision and firmware version remain unknown.
- User accepted replacing the flight firmware; ArduCopter is the planned stack,
  with the exact board target and migration still to be verified/performed.
- Luckfox has not yet been configured.
- Initial tests: cooperative target drone, controlled environment, human supervision;
  evaluate tracking accuracy, stability, and system performance.
- Separation, speed envelope, lens, capture resolution, frame rate, and firmware
  versions remain unspecified.

## Verified platform facts

Luckfox identifies the Max as RV1106G3, Cortex-A7, with 256 MB DDR3L and a two-lane
MIPI CSI camera interface. This is a constrained embedded deployment target; the
workstation CPU implementation has no measured performance on it.
[Luckfox board documentation](https://wiki.luckfox.com/Luckfox-Pico-Pro-Max)

Luckfox explicitly lists MIS5001 as a supported 5 MP camera on RV1106 boards,
including the Max. Its camera tutorial specifies Buildroot and does not cover Ubuntu.
Module support does not establish the actual capture mode, lens calibration, or
frame rate of the user's installed image.
[Luckfox camera documentation](https://wiki.luckfox.com/Luckfox-Pico-Pro-Max/CSI-Camera/)

The documented NPU workflow converts models with RKNN-Toolkit2 on a development
computer and deploys the converted model using a board runtime. We must match
toolkit/runtime versions and verify NanoTrack operator support, tensor layout, and
quantization accuracy; successful conversion is not yet established.
[Luckfox RKNN documentation](https://wiki.luckfox.com/Luckfox-Pico-Pro-Max/RKNN/)

ArduPilot documents the DAKEFPV H743/H743 Pro and its UART mapping. That does not
identify which firmware or board revision is actually installed. A MAVLink interface
is conditional on confirming the selected firmware and serial-port configuration.
Do not apply settings for a similar H743 board by assumption.
[ArduPilot board documentation](https://en.ardupilot.org/plane/docs/common-dakefpvh743pro.html)

## Selected integration direction

Recommended starting system: official Luckfox **Buildroot**, choosing the
**Pico Pro/Max SPI NAND** image for the onboard flash. The current download guide
also supports TF-card boot on Pro/Max; storage and image must match. Record the
downloaded image filename/date and checksum so subsequent tests are reproducible.
[Official image selection](https://wiki.luckfox.com/Luckfox-Pico-RV1106/Getting-Started/overview/)
[Flashing instructions, including macOS](https://wiki.luckfox.com/Luckfox-Pico-RV1106/Getting-Started/Image-flashing/)

Use the matching Luckfox SDK and its cross-toolchain for the native application.
Luckfox recommends Ubuntu 22.04 as the SDK development host; this is separate
from Buildroot running on the Pico. Preserve the SDK commit and match deployed
media/NPU libraries before building the adapter.
[Official SDK](https://github.com/LuckfoxTECH/luckfox-pico)

Keep the current flight firmware for initial manual-flight perception recording.
For later companion-controlled multirotor following, the recommended flight stack
is ArduCopter, subject to verifying the exact board target and sensors. ArduPilot
documents companion computers and Guided movement commands. This recommendation
has been accepted by the user; no firmware flash has been performed. Betaflight's 2026.6 release notes
describe its new autonomous waypoint features as simulation-only, so their presence
does not establish suitability for this project.
[ArduPilot Guided mode](https://ardupilot.org/copter/docs/ac2_guidedmode.html)
[Betaflight 2026.6 release notes](https://betaflight.com/docs/wiki/release/Betaflight-2026-6-Release-Notes)

1. Install the selected official image, then verify the sensor and capture mode.
   Establish native camera/ISP capture with frame sequence numbers and documented
   timestamp semantics using the matching SDK. Confirm exposure timing separately
   from delivery timing.
2. Implement a native camera adapter using the board's camera/media SDK. Retain
   source detail for localization; obtain model-sized crops using available hardware
   conversion/scaling. Explicitly release every acquired frame buffer, including
   error paths. Keep a bounded latest-frame policy.
3. Convert and evaluate the existing NanoTrack networks for RV1106. Compare their
   intermediate/final outputs with the preserved ONNX baseline. Measure on-board
   latency and memory before choosing processing resolution, optical-flow point
   count, recovery search scope, and a deadline. Full-frame recovery and workstation
   optical-flow settings must not be assumed affordable.
4. Port the Python evidence checks, loss/reselection behavior, and observation-age
   contract into the native runtime. The existing C++ template tracker is a separate
   baseline: compiling it does not port the Python NanoTrack implementation.
5. Log frame IDs, capture/result timestamps and clock domains, image dimensions,
   accepted/rejected state, source-image box coordinates, and fault reasons.
   Use this output for supervised perception tests before adding flight commands.
6. After firmware identification, implement and simulate the flight-controller
   interface with an independent timeout. Range/relative-state estimation and
   separation control are separate unfinished requirements, not derivable from
   bounding-box confidence alone.

There is no board SDK, cross-toolchain, converted RKNN model, board connection, or
verified camera adapter in this workspace. The existing `tools/live_tracking.py`
is a desktop OpenCV bench runner and is **not** the MIS5001/RKMPI integration.

The [native NanoTrack core](NATIVE_TRACKER.md) now implements tensor decoding and
the observation/evidence state machine in portable C++17. Real ONNX output parity,
quantized/strided buffer handling, and native fault/recovery behaviors are covered
by host tests. This is an implemented component of steps 3–4 above; model execution
and camera/visual-evidence adapters remain outstanding.

## Read-only board inventory

Copy `tools/inspect_luckfox.sh` onto the Luckfox using your normal transfer method,
then run there:

```sh
sh inspect_luckfox.sh > luckfox-inventory.txt
```

The script reads OS, device-tree, memory, camera device names, existing sensor names,
common runtime library paths, and UART device names. It does not start capture,
probe the I2C bus, change configuration, open serial ports, flash firmware, or install
packages. It requires no Python. Review and share the output to determine the next
adapter/build step. Library names alone do not verify ABI compatibility.

Also record the flight-controller firmware name/version, exact camera label/lens,
and the image/SDK release used to flash the Luckfox. Camera mode and exposure-to-result
timing must subsequently be measured; this inventory does not measure either.
