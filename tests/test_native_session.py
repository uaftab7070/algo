"""Run the dependency-free C++ state/decoder checks through normal discovery."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class NativeSessionChecks(unittest.TestCase):
    def test_native_core(self):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('A C++17 compiler is required for native session tests')
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / 'nanotrack-core-test')
            build = subprocess.run([compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic',
                            '-pthread', '-I', str(root / 'include'),
                            str(root / 'src/nanotrack_decoder.cpp'),
                            str(root / 'src/nanotrack_session.cpp'),
                            str(root / 'tests/native/nanotrack_core_test.cpp'), '-o', executable],
                           capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([executable], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('21 native checks passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
