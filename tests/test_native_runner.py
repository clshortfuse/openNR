from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from benchmarks.native.run import stage


class NativeRunnerTests(unittest.TestCase):
    def test_stages_root_multiconfiguration_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / 'build/tools/bin/Release/dlssnr-d3d11-repro.exe'
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b'test executable')
            candidate = root / 'nvngx_dlssnr.dll'
            core = root / 'nvngx.dll'
            candidate.write_bytes(b'candidate')
            core.write_bytes(b'core')
            with patch('benchmarks.native.run.__file__', str(root / 'benchmarks/native/run.py')):
                record = stage('d3d11', candidate, core, root / 'result')
            self.assertEqual(Path(record['command'][0]).name, executable.name)

    def test_stages_without_execution_and_refuses_existing_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / 'build/benchmarks/native/bin/dlssnr-repro.exe'
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b'test executable')
            files = [root / name for name in ('nvngx_dlssnr.dll', 'nvngx.dll', 'cupti.dll')]
            for file in files:
                file.write_bytes(file.name.encode())
            with patch('benchmarks.native.run.__file__', str(root / 'benchmarks/native/run.py')):
                record = stage('d3d12', files[0], files[1], root / 'result', files[2])
                self.assertFalse(record['executed'])
                self.assertEqual(len(record['files']), 4)
                self.assertEqual(record['command'][-1], '8')
                for file in files:
                    self.assertEqual((root / 'result' / file.name).read_bytes(), file.read_bytes())
                with self.assertRaises(FileExistsError):
                    stage('d3d12', files[0], files[1], root / 'result', files[2])

    def test_missing_runtime_rejected_before_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaises(ValueError):
                stage('d3d12', root / 'missing.dll', root / 'core.dll', root / 'result')
            self.assertFalse((root / 'result').exists())