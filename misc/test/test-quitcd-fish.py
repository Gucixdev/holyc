#!/usr/bin/env python3
"""Run with python3 misc/test/test-quitcd-fish.py (requires fish on PATH)."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


FISH = shutil.which(os.environ.get("FISH", "fish"))
WRAPPER = Path(__file__).resolve().parents[1] / "quitcd" / "quitcd.fish"


@unittest.skipUnless(FISH, "fish is required")
class QuitcdFishTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.home = self.root / "home"
        self.home.mkdir()
        self.config = self.root / "config space\\"
        (self.config / "nnn").mkdir(parents=True)
        self.lastd = self.config / "nnn" / ".lastd"
        self.fixture = self.root / "lastd-fixture"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        stub = self.bin / "nnn"
        stub.write_text('#!/bin/sh\n'
                        'if test -e "$NNN_TEST_LASTD"; then\n'
                        '    cp -- "$NNN_TEST_LASTD" "$NNN_TMPFILE"\n'
                        'fi\n')
        stub.chmod(0o755)
        self.env = {"PATH": str(self.bin) + os.pathsep + os.defpath,
                    "HOME": str(self.home), "XDG_CONFIG_HOME": str(self.config),
                    "NNN_TEST_LASTD": str(self.fixture), "LC_ALL": "C.UTF-8"}
        # Preserve a caller's library path when testing an isolated fish build.
        if "LD_LIBRARY_PATH" in os.environ:
            self.env["LD_LIBRARY_PATH"] = os.environ["LD_LIBRARY_PATH"]

    def run_wrapper(self):
        result = subprocess.run(
            [FISH, "--no-config", "-c",
             'source "$argv[1]"; n; printf "%s\\0" "$PWD"', str(WRAPPER)],
            cwd=self.root, env=self.env, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.lastd.exists())
        return result

    def write_lastd(self, target):
        # write_lastdir() emits a POSIX single-quoted cd command, without newline.
        self.fixture.write_text("cd '" + str(target).replace("'", "'\\''") + "'")

    def test_paths(self):
        for name in ("plain", "with spaces", "trailing\\", "double\\\\slash",
                     "quote'", "slash\\'quote", "tab\there", "中文",
                     "line\nbreak", "trailing\n", "trailing\n\n", "$dollar;*()"):
            with self.subTest(name=name):
                target = self.root / name
                target.mkdir()
                self.write_lastd(target)
                result = self.run_wrapper()
                self.assertEqual(result.stdout, os.fsencode(target) + b"\0")
                self.assertEqual(result.stderr, b"")

    def test_missing_directory_does_not_change_directory(self):
        self.write_lastd(self.root / "missing")
        result = self.run_wrapper()
        self.assertEqual(result.stdout, os.fsencode(self.root) + b"\0")
        self.assertNotEqual(result.stderr, b"")

    def test_no_lastd_does_not_change_directory(self):
        result = self.run_wrapper()
        self.assertEqual(result.stdout, os.fsencode(self.root) + b"\0")
        self.assertEqual(result.stderr, b"")


if __name__ == "__main__":
    unittest.main()
