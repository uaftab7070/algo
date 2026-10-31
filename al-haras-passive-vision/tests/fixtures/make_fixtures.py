#!/usr/bin/env python3
"""Generates the deterministic synthetic Y4M test corpus (DAT-007 development set).

These files are synthetic fixtures for capture/replay/annotation reliability
tests. They are marked XAHPV_SYNTHETIC=1 so the application records their
origin; they are NOT authorized real scenes and do not replace the hardware
acceptance corpus described in docs/known-limitations.md.

Usage: make_fixtures.py OUTPUT_DIR   (Python 3 standard library only)
"""
import hashlib
import json
import os
import sys

W, H = 160, 120
FPS = (30, 1)


def lcg(seed):
    state = seed & 0xFFFFFFFF
    while True:
        state = (1103515245 * state + 12345) & 0x7FFFFFFF
        yield state >> 16


def header(width=W, height=H, colorspace='420jpeg', extra=''):
    return (f'YUV4MPEG2 W{width} H{height} F{FPS[0]}:{FPS[1]} Ip A1:1 C{colorspace} XAHPV_SYNTHETIC=1{extra}\n').encode()


def frame_i420(luma_fn, u=128, v=128, width=W, height=H):
    y = bytearray(width * height)
    for row in range(height):
        base = row * width
        for col in range(width):
            y[base + col] = max(0, min(255, int(luma_fn(col, row))))
    chroma = ((width + 1) // 2) * ((height + 1) // 2)
    return b'FRAME\n' + bytes(y) + bytes([u]) * chroma + bytes([v]) * chroma


def write(path, frames, head=None):
    with open(path, 'wb') as out:
        out.write(head or header())
        for f in frames:
            out.write(f)


def static_scene(n=60):
    pattern = lambda x, y: 60 + ((x // 16 + y // 16) % 2) * 120
    data = frame_i420(pattern, 110, 140)
    return [data] * n


def bench_motion(n=90):
    frames = []
    for i in range(n):
        bx, by = 10 + i, 40 + (i % 20)
        frames.append(frame_i420(lambda x, y, bx=bx, by=by: 200 if bx <= x < bx + 24 and by <= y < by + 24 else 70 + (x % 7), 120, 136))
    return frames


def illumination(n=90):
    frames = []
    for i in range(n):
        level = 4 + i * 2.8  # Dark to bright: exercises very_dark/very_bright flags.
        frames.append(frame_i420(lambda x, y, level=level: level + ((x + y) % 9) - 4))
    return frames


def clutter(n=60, seed=7):
    rng = lcg(seed)
    frames = []
    for _ in range(n):
        noise = [next(rng) & 0xFF for _ in range(W * H)]
        frames.append(frame_i420(lambda x, y, noise=noise: noise[y * W + x], next(rng) & 0xFF, next(rng) & 0xFF))
    return frames


def empty_scene(n=45):
    return [frame_i420(lambda x, y: 128)] * n


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    corpus = {
        'static.y4m': ('static checkerboard', static_scene()),
        'bench_motion.y4m': ('block moved across a textured bench', bench_motion()),
        'illumination.y4m': ('global illumination ramp dark to bright', illumination()),
        'clutter.y4m': ('high-entropy cluttered texture', clutter()),
        'empty.y4m': ('uniform empty scene (flat, low contrast)', empty_scene()),
    }
    manifest = {'schema_version': '1.0', 'generator': 'tests/fixtures/make_fixtures.py', 'synthetic': True,
                'note': 'Development fixtures only; not hardware acceptance footage.', 'files': {}}
    for name, (purpose, frames) in corpus.items():
        write(os.path.join(out, name), frames)
        manifest['files'][name] = {'purpose': purpose, 'frames': len(frames)}
    # Malformed inputs for AC-011.
    good = bench_motion(12)
    with open(os.path.join(out, 'corrupt_header.y4m'), 'wb') as f:
        f.write(header())
        for i, fr in enumerate(good):
            f.write(b'FRAXE\n' + fr[6:] if i == 6 else fr)
    manifest['files']['corrupt_header.y4m'] = {'purpose': 'malformed FRAME marker at frame 6', 'frames': 12}
    with open(os.path.join(out, 'truncated.y4m'), 'wb') as f:
        f.write(header())
        for fr in good[:8]:
            f.write(fr)
        f.write(good[8][: len(good[8]) // 2])
    manifest['files']['truncated.y4m'] = {'purpose': 'file cut in the middle of frame 8', 'frames': 8}
    with open(os.path.join(out, 'unsupported_422.y4m'), 'wb') as f:
        f.write(header(colorspace='422'))
        f.write(b'FRAME\n' + bytes(W * H * 2))
    manifest['files']['unsupported_422.y4m'] = {'purpose': 'unsupported 4:2:2 colorspace', 'frames': 1}
    with open(os.path.join(out, 'interlaced.y4m'), 'wb') as f:
        f.write(header().replace(b' Ip ', b' It '))
    manifest['files']['interlaced.y4m'] = {'purpose': 'unsupported interlaced header', 'frames': 0}
    rng = lcg(99)
    with open(os.path.join(out, 'not_video.bin'), 'wb') as f:
        f.write(bytes(next(rng) & 0xFF for _ in range(4096)))
    manifest['files']['not_video.bin'] = {'purpose': 'random bytes, not Y4M', 'frames': 0}
    open(os.path.join(out, 'empty_file.y4m'), 'wb').close()
    manifest['files']['empty_file.y4m'] = {'purpose': 'zero-length file', 'frames': 0}
    for name in manifest['files']:
        with open(os.path.join(out, name), 'rb') as f:
            manifest['files'][name]['sha256'] = hashlib.sha256(f.read()).hexdigest()
    with open(os.path.join(out, 'corpus-manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
        f.write('\n')


if __name__ == '__main__':
    main()
