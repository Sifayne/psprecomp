#!/usr/bin/env python3
"""Integration checks for the built FFmpeg dependency; no game data required.

Needs a build from player/ffmpeg.py. FFMPEG_DEPS names its --deps directory
when that is not this checkout's build/deps (Last Raven: <repo>/build/deps)."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("player_ffmpeg", Path(__file__).resolve().parents[1] / "ffmpeg.py")
ffmpeg = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ffmpeg)
if os.environ.get("FFMPEG_DEPS"):
    ffmpeg.DEPS = Path(os.environ["FFMPEG_DEPS"]).resolve()
    ffmpeg.PREFIX = ffmpeg.DEPS / "ffmpeg"


class FFmpegDistributionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        ffmpeg.verify(ffmpeg.PREFIX)
        cls.scratch = tempfile.TemporaryDirectory(prefix="lr-ffmpeg-tests-")
        cls.root = Path(cls.scratch.name)
        cls.bundle = cls.root / "relocated app with spaces"
        ffmpeg.bundle(ffmpeg.PREFIX, cls.bundle)

    @classmethod
    def tearDownClass(cls):
        cls.scratch.cleanup()

    def test_relocated_executable_uses_bundled_libraries(self):
        source = self.root / "probe.c"
        source.write_text(
            '#include <stdio.h>\n#include <libavcodec/avcodec.h>\n'
            '#include <libavutil/avutil.h>\n'
            'int main(void) { puts(avcodec_license()); puts(avutil_license());\n'
            'return !avcodec_find_decoder_by_name("atrac3") ||\n'
            '!avcodec_find_decoder_by_name("atrac3plus"); }\n')
        executable = self.bundle / "audio-probe"
        subprocess.run(["cc", str(source), "-I" + str(ffmpeg.PREFIX / "include"),
                        str(ffmpeg.PREFIX / "lib/libavcodec.so"),
                        str(ffmpeg.PREFIX / "lib/libavutil.so"),
                        "-Wl,-rpath,$ORIGIN/lib", "-o", str(executable)], check=True)
        env = os.environ.copy()
        env.pop("LD_LIBRARY_PATH", None)
        result = subprocess.run([str(executable)], cwd="/tmp", env=env,
                                capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout.splitlines(), [ffmpeg.LICENSE, ffmpeg.LICENSE])
        self.assertNotIn(str(ffmpeg.ROOT), ffmpeg.dynamic(executable))
        report = ffmpeg.verify(self.bundle)
        for name in ("avcodec", "avutil"):
            self.assertEqual(Path(report[name]["path"]).parent, self.bundle / "lib")

    def test_matching_source_and_license_are_staged(self):
        source = self.bundle / "source" / f"ffmpeg-{ffmpeg.PIN['version']}.tar.xz"
        self.assertEqual(ffmpeg.digest(source), ffmpeg.PIN["sha256"])
        self.assertIn("GNU LESSER GENERAL PUBLIC LICENSE",
                      (self.bundle / "licenses/ffmpeg/COPYING.LGPLv2.1").read_text())
        self.assertEqual(ffmpeg.digest(self.bundle / "source/rebuild/player/ffmpeg.py"),
                         ffmpeg.digest(Path(ffmpeg.__file__)))
        subprocess.run(["sha256sum", "--check", "SHA256SUMS"], cwd=self.bundle,
                       check=True, capture_output=True)

    def test_corrupt_cached_source_is_rejected(self):
        deps = self.root / "bad-source"
        downloads = deps / "downloads"
        downloads.mkdir(parents=True)
        (downloads / f"ffmpeg-{ffmpeg.PIN['version']}.tar.xz").write_bytes(b"wrong source")
        with patch.object(ffmpeg, "DEPS", deps):
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                ffmpeg.archive()

    def test_missing_library_is_rejected(self):
        broken = self.root / "missing-library"
        shutil.copytree(self.bundle / "lib", broken / "lib", symlinks=True)
        (broken / "lib/libavcodec.so").unlink()
        with self.assertRaises(subprocess.CalledProcessError):
            ffmpeg.verify(broken)

    def test_dependency_outside_bundle_is_rejected(self):
        broken = self.root / "external-library"
        shutil.copytree(self.bundle / "lib", broken / "lib", symlinks=True)
        util = broken / "lib" / f"libavutil.so.{ffmpeg.PIN['avutil_major']}"
        util.unlink()
        util.symlink_to(ffmpeg.PREFIX / "lib" / util.name)
        with self.assertRaises(subprocess.CalledProcessError) as raised:
            ffmpeg.verify(broken)
        self.assertIn("resolved outside the bundle", raised.exception.stderr)

    def test_existing_bundle_is_preserved(self):
        before = (self.bundle / "manifest.json").read_bytes()
        with self.assertRaisesRegex(ValueError, "already exists"):
            ffmpeg.bundle(ffmpeg.PREFIX, self.bundle)
        self.assertEqual(before, (self.bundle / "manifest.json").read_bytes())


if __name__ == "__main__":
    unittest.main()
