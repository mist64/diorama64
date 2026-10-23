import pathlib
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import sm64ar_common as common  # noqa: E402


class PackHelperTests(unittest.TestCase):
    def test_resize_cmd_linux(self):
        with mock.patch.object(common, "IS_MAC", False):
            self.assertEqual(common.resize_png_cmd("a.png", "b.png", 512), ["magick", "png:a.png", "-resize", "512x512>", "png:b.png"])
            self.assertEqual(common.resize_png_cmd("a.png", "b.png", None), ["magick", "png:a.png", "png:b.png"])

    def test_resize_cmd_mac(self):
        with mock.patch.object(common, "IS_MAC", True):
            self.assertEqual(common.resize_png_cmd("a.png", "b.png", 512), ["/usr/bin/sips", "-Z", "512", "a.png", "--out", "b.png"])

    def test_generator_cmd_linux(self):
        with mock.patch.object(common, "IS_MAC", False):
            cmd = common.pack_generator_cmd(pathlib.Path("/r/rom.z64"), pathlib.Path("/tmp/x"))
            self.assertEqual(cmd[1:], ["--rom", "/r/rom.z64", "--res", str(common.SRC / "build" / "us_pc")])
            self.assertTrue(cmd[0].endswith("sm64ar_pack_tool"))

    def test_make_program(self):
        with mock.patch("shutil.which", lambda n: "/x/gmake" if n == "gmake" else None):
            self.assertEqual(common.make_program(), "gmake")
        with mock.patch("shutil.which", lambda n: None):
            self.assertEqual(common.make_program(), "make")
