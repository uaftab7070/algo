# Native NanoTrack perception core

This C++17 library is the first implemented part of the Pico Max native port. It
contains a head decoder and a synchronized observation/evidence state machine.
It builds without OpenCV, Python, or an RKNN SDK. Camera capture, model execution,
crop generation, appearance/flow measurement, and model conversion remain adapter
work. The library has been tested on the development Mac, not on RV1106 hardware.

## Build and verification

With CMake 3.16+ and a C++17 toolchain:

```sh
cmake -S . -B build/native -DRV1106_BUILD_OPENCV_BASELINE=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/native -j
(cd build/native && ctest --output-on-failure)
```

The library target is `nanotrack_core`; the executable test target is
`nanotrack-core-test`. The existing OpenCV baseline is still enabled by default
for the original build command. For cross-compilation, supply a toolchain file
from the selected Luckfox SDK and do not attempt to execute ARM tests on the host.

The normal Python test discovery also compiles the native code with `clang++` or
`g++` and runs its state tests. It compares the decoder with actual local ONNX
inference outputs across translation, missing targets, and search expansion:

```sh
.venv/bin/python -m unittest discover -s tests -p 'test_native_*.py' -v
```

Compiler-dependent tests explicitly skip when neither compiler is available.
They must run without skips in the native release environment. CMake itself is
not installed in the current workspace, so local verification uses direct compiler
invocation; the CMake build above still needs verification in the SDK environment.

## Model and tensor contract

The local models produced these float32 NCHW shapes in development checks:

| Stage | Input / output |
|---|---|
| Template backbone | RGB crop 127x127 -> features `[1,48,8,8]` |
| Search backbone | RGB crop 255x255 -> features `[1,48,16,16]` |
| Head `input1` | Immutable selected-template features |
| Head `input2` | Current search features |
| Head `output1` | `[1,2,16,16]`: background, foreground logits |
| Head `output2` | `[1,4,16,16]`: left, top, right, bottom distances |

The decoder accepts float32, affine INT8, and affine UINT8 tensors in NCHW or NHWC.
The caller supplies logical channel counts, buffer sizes, and element strides.
Rows, planes, and NHWC channel padding are supported. Unsupported formats, short
buffers, overlapping/overflowing strides, invalid quantization, and nonfinite
outputs are rejected with `std::invalid_argument`. NC1HWC2 and float16 require an
explicit adapter conversion. Query actual runtime output attributes; never treat
a padded or blocked native output buffer as a packed float array.

Model conversion can change names, order, formats, and strides. Verify each output
against its role and shape. The 127 and 255 backbone inputs may need separately
converted fixed-shape models. No successful RV1106 conversion has been established.
The preserved Python ONNX implementation remains the numerical reference.

The decoder ranks proposals using scale/aspect penalties and a Hann position
window, but reports the raw classification score without that preference. PSR and
spatially separate competing boxes are also reported. Decoding does not commit a
track or update template features.

Affine-layout tests compare the native decoder with explicitly dequantized
versions of the same tensors. They verify buffer handling, **not** the accuracy of
an RKNN-quantized model. That requires converted models and recorded camera data.

## Session and camera adapter contract

1. Construct `nano::Session` with a positive, explicit `max_observation_age_us`.
   All thresholds are development settings and require aerial-footage evaluation.
2. Initialize using a manually selected box, actual image dimensions, a source
   epoch, and a source frame ID/capture timestamp. The selected box must be fully
   inside the frame and at least 12 pixels in each dimension.
3. Keep the returned `selection_id` with every inference job. After inference,
   call `decode`, measure appearance/texture/flow on that same frame and proposal,
   then call `submit` with the saved selection ID and the current monotonic time
   at submission (which includes inference and any result-delivery delay).
   A previous selection's result or model failure cannot affect the new selection.
4. Poll `snapshot` independently from camera and model execution. Its short mutex
   protects only state changes. Model execution and visual measurement must remain
   outside this mutex so the consumer can observe expiry during stalled inference.
5. Call `camera_failure` on capture failure; call `model_failure(selection_id)`
   when a model/decoder operation fails. These faults require manual reselection.

Use **microseconds from one monotonic clock domain** for capture, completion, and
polling. Frame IDs and capture timestamps must both increase. A new source epoch
must be issued on a camera restart, even if frame IDs reset. Exposure timestamps
need verified mapping to this clock; delivery timestamps alone cannot establish
sensor-to-result age. A downstream consumer also needs an independent message
timeout for process/computer failure.

`VisualEvidence` is supplied by a real vision adapter: appearance similarities,
original-target similarity, texture correlation, motion agreement, and optional
flow support. Supplying constant scores or assumed flow would bypass the visual
checks. The adapter producing these values is not implemented in this native
library; the Python tracker contains the reference image-processing operations.

## Acceptance and recovery behavior

- Initial selection and recovery require three consecutive, spatially consistent,
  strongly supported proposals by default. Duplicate/out-of-order frames cannot
  advance confirmation or refresh observation age.
- Confidence combines raw neural score, PSR, appearance, and measured flow/motion
  agreement. It remains a heuristic score, not an identity probability.
- Rejected proposals do not move the last accepted box. Loss uses elapsed capture
  time instead of a frame counter, so processing rate does not define loss time.
- Ambiguous competing targets are rejected even with flow support. Fully visible
  boxes are required; partial-target acceptance is deferred until validated.
- Source changes, invalid numeric evidence, expiry, camera/model failure, and clock
  rollback invalidate acceptance and require reselection. A late result cannot
  clear a latched fault.
- `has_box` and `last_box` retain historical localization after loss. Only
  `accepted=true` identifies a valid current observation. The library emits image
  observations, with no distance estimate or flight-control commands.

The native session is deliberately more conservative than the Python reference:
it also confirms initial selection, rejects ambiguous matches unconditionally,
and requires complete in-frame geometry. It does not yet implement reference-patch
search, verified flow-only bridging, or template adaptation. Those modules require
native visual measurements and target-board profiling before integration.
