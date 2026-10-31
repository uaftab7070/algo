"""Cross-language checks using real ONNX outputs, not prerecorded golden guesses."""
import ctypes as ct
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import cv2
import numpy as np

from precision_tracker import NanoMatcher

ROOT = Path(__file__).resolve().parents[1]


class ViewSpec(ct.Structure):
    _fields_ = [('data', ct.c_void_p), ('bytes', ct.c_size_t),
                ('channels', ct.c_int), ('height', ct.c_int), ('width', ct.c_int),
                ('type', ct.c_int), ('layout', ct.c_int),
                ('row', ct.c_size_t), ('plane', ct.c_size_t), ('pixel', ct.c_size_t),
                ('scale', ct.c_float), ('zero_point', ct.c_int)]


def tensor(array, channels, layout=0, scale=1, zero_point=0, **strides):
    spec = ViewSpec(array.ctypes.data, array.nbytes, channels, 16, 16,
                    {np.dtype('float32'): 0, np.dtype('int8'): 1, np.dtype('uint8'): 2}[array.dtype],
                    layout, strides.get('row', 0), strides.get('plane', 0), strides.get('pixel', 0),
                    scale, zero_point)
    # Keep storage alive throughout the C call.
    spec.storage = array
    return spec


class RecordingHead:
    def __init__(self, net):
        self.net = net

    def setInput(self, *args):
        return self.net.setInput(*args)

    def forward(self, *args):
        self.outputs = self.net.forward(*args)
        return self.outputs


class NativeDecoderChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            raise unittest.SkipTest('A C++17 compiler is required for native decoder tests')
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        library = Path(cls.temp.name) / 'decoder.so'
        build = subprocess.run([compiler, '-std=c++17', '-O2', '-shared', '-fPIC', '-Wall', '-Wextra',
                        '-Werror', '-pedantic', '-I', str(ROOT / 'include'),
                        str(ROOT / 'src/nanotrack_decoder.cpp'),
                        str(ROOT / 'tests/native/nanotrack_bridge.cpp'), '-o', str(library)],
                       capture_output=True, text=True, timeout=60)
        if build.returncode:
            raise RuntimeError(f'Native decoder compilation failed:\n{build.stderr}')
        cls.library = ct.CDLL(str(library))
        cls.native = cls.library.nano_decode_for_test
        cls.native.argtypes = [ct.POINTER(ViewSpec), ct.POINTER(ViewSpec), ct.POINTER(ct.c_float),
                               ct.c_float, ct.POINTER(ct.c_float), ct.c_char_p, ct.c_size_t]
        cls.native.restype = ct.c_int
        cv2.setNumThreads(2)

    def decode(self, cls, dist, box=(140, 65, 60, 90), expansion=1):
        output = (ct.c_float * 9)()
        error = ct.create_string_buffer(256)
        status = self.native(ct.byref(cls), ct.byref(dist), (ct.c_float * 4)(*box),
                             expansion, output, error, len(error))
        if status:
            raise ValueError(error.value.decode())
        return np.array(output)

    def data(self):
        rng = np.random.default_rng(2026)
        cls = rng.normal(0, 2, (2, 16, 16)).astype(np.float32)
        dist = rng.uniform(10, 100, (4, 16, 16)).astype(np.float32)
        return cls, dist

    def test_real_model_outputs_match_python_for_motion_loss_and_search_expansion(self):
        rng = np.random.default_rng(71)
        patch = rng.integers(0, 256, (90, 60, 3), dtype=np.uint8)
        def frame(shift=0, covered=False):
            image = np.full((240, 400, 3), (35, 100, 40), np.uint8)
            if not covered:
                image[65:155, 140 + shift:200 + shift] = patch
            return image
        matcher = NanoMatcher()
        matcher.head = RecordingHead(matcher.head)
        box = np.array([140, 65, 60, 90], np.float32)
        matcher.initialize(frame(), box)
        original = matcher.template_features.copy()
        for expansion in (1, 1.15, 1.45):
            for shift, covered in ((0, False), (4, False), (28, False), (0, True)):
                with self.subTest(expansion=expansion, shift=shift, covered=covered):
                    measured, score, psr, ambiguous = matcher.infer(frame(shift, covered), box, expansion)
                    cls, dist = matcher.head.outputs
                    actual = self.decode(tensor(cls, 2), tensor(dist, 4), box, expansion)
                    np.testing.assert_allclose(actual[:4], measured, atol=0.002, rtol=1e-5)
                    np.testing.assert_allclose(actual[4:6], [score, psr], atol=2e-5, rtol=1e-5)
                    self.assertEqual(bool(actual[6]), ambiguous)
        np.testing.assert_array_equal(original, matcher.template_features)

    def test_nhwc_matches_nchw(self):
        cls, dist = self.data()
        expected = self.decode(tensor(cls, 2), tensor(dist, 4))
        cls = np.ascontiguousarray(cls.transpose(1, 2, 0))
        dist = np.ascontiguousarray(dist.transpose(1, 2, 0))
        np.testing.assert_array_equal(self.decode(tensor(cls, 2, 1), tensor(dist, 4, 1)), expected)

    def test_padding_is_not_decoded_as_data(self):
        cls, dist = self.data()
        expected = self.decode(tensor(cls, 2), tensor(dist, 4))
        for layout in (0, 1):
            specs = []
            for array, channels in ((cls, 2), (dist, 4)):
                if layout == 0:
                    padded = np.full((channels, 18, 20), np.nan, np.float32)
                    padded[:, :16, :16] = array
                    specs.append(tensor(padded, channels, row=20, plane=18 * 20))
                else:
                    padded = np.full((16, 20, 8), np.nan, np.float32)
                    padded[:, :16, :channels] = array.transpose(1, 2, 0)
                    specs.append(tensor(padded, channels, 1, row=20 * 8, pixel=8))
            with self.subTest(layout=layout):
                np.testing.assert_array_equal(self.decode(*specs), expected)

    def test_affine_quantization_matches_explicit_dequantization(self):
        cls, dist = self.data()
        for dtype, zero_point in ((np.int8, -3), (np.uint8, 123)):
            quantized, dequantized = [], []
            for data, channels, scale in ((cls, 2, .125), (dist, 4, 1.0)):
                limits = np.iinfo(dtype)
                q = np.clip(np.rint(data / scale + zero_point), limits.min, limits.max).astype(dtype)
                quantized.append(tensor(q, channels, scale=scale, zero_point=zero_point))
                dequantized.append(tensor((q.astype(np.float32) - zero_point) * scale, channels))
            with self.subTest(dtype=dtype):
                np.testing.assert_array_equal(self.decode(*quantized), self.decode(*dequantized))

    def test_invalid_tensor_contract_is_rejected(self):
        cls, dist = self.data()
        cases = [('channels', 3), ('height', 15), ('width', 17), ('bytes', 1),
                 ('layout', 99), ('type', 99), ('row', 2), ('plane', 2),
                 ('pixel', 3), ('row', ct.c_size_t(-1).value), ('data', None)]
        for field, value in cases:
            invalid = tensor(cls, 2)
            setattr(invalid, field, value)
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.decode(invalid, tensor(dist, 4))

    def test_nonfinite_values_are_rejected_even_away_from_peak(self):
        cls, dist = self.data()
        for value in (float('nan'), float('inf'), float('-inf')):
            for data in (cls, dist):
                old = data[0, 0, 0]
                data[0, 0, 0] = value
                with self.subTest(value=value, channels=len(data)), self.assertRaises(ValueError):
                    self.decode(tensor(cls, 2), tensor(dist, 4))
                data[0, 0, 0] = old

    def test_invalid_anchor_and_quantization_are_rejected(self):
        cls, dist = self.data()
        for box, expansion in (((0, 0, 0, 10), 1), ((0, 0, 10, -1), 1),
                               ((float('nan'), 0, 10, 10), 1), ((0, 0, 10, 10), 0)):
            with self.subTest(box=box, expansion=expansion), self.assertRaises(ValueError):
                self.decode(tensor(cls, 2), tensor(dist, 4), box, expansion)
        q = cls.astype(np.int8)
        for scale, zero_point in ((0, 0), (-1, 0), (float('nan'), 0), (1, 128)):
            with self.subTest(scale=scale, zero_point=zero_point), self.assertRaises(ValueError):
                self.decode(tensor(q, 2, scale=scale, zero_point=zero_point), tensor(dist, 4))


if __name__ == '__main__':
    unittest.main()
