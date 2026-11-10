#!/usr/bin/env python3
"""Validate the exact in-process gzip reader, including CRC and truncation refusal."""
import ctypes
import gzip
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class InflateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="track-analysis-inflate-")
        cls.root = Path(cls.directory.name)
        library = cls.root / "inflate.so"
        subprocess.run([os.environ.get("HOST_CC", "clang"),
                        "-dynamiclib" if sys.platform == "darwin" else "-shared", "-fPIC", "-O2",
                        str(ROOT / "inflate.c"), str(ROOT / "vendor/miniz/miniz_tinfl.c"),
                        "-o", str(library)], check=True)
        cls.library = ctypes.CDLL(str(library))
        cls.library.ta_unpack_worker.argtypes = [ctypes.c_char_p, ctypes.c_int]
        cls.library.ta_unpack_worker.restype = ctypes.c_int

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def unpack(self, data):
        source = self.root / "input.gz"
        source.write_bytes(data)
        with (self.root / "output").open("wb") as out:
            result = self.library.ta_unpack_worker(os.fsencode(source), out.fileno())
        return result, (self.root / "output").read_bytes()

    def test_multiple_dictionary_windows(self):
        data = bytes(range(256)) * 700 + b"Track Analysis\n"
        result, decoded = self.unpack(gzip.compress(data, mtime=0))
        self.assertEqual(result, 1)
        self.assertEqual(decoded, data)

    def test_bad_crc_and_truncation(self):
        data = bytearray(gzip.compress(b"worker" * 500, mtime=0))
        data[-8] ^= 0x80
        self.assertEqual(self.unpack(data)[0], 0)
        self.assertEqual(self.unpack(data[:-5])[0], 0)

    def test_invalid_header(self):
        self.assertEqual(self.unpack(b"invalid-gzip" * 5)[0], 0)
        self.assertEqual(self.unpack(b"")[0], 0)

if __name__ == "__main__":
    unittest.main()
