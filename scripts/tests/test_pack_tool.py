"""The Linux pack tool's error paths (no ROM needed). Skipped until build/host has built it.

The tool checks, in order: the ROM file exists, the resource directory exists, the --generate-tex input holds a .png,
then the ROM is accepted (init). Every failure prints "error:", exits 1 and creates no output directory.
"""
import pathlib
import subprocess
import tempfile
import unittest

TOOL = pathlib.Path(__file__).resolve().parents[2] / "build" / "host" / "pack_tool" / "sm64ar_pack_tool"


class PackToolErrorTests(unittest.TestCase):
    def setUp(self):
        if not TOOL.exists():
            raise unittest.SkipTest(f"{TOOL} not built")
        self._tmp = tempfile.TemporaryDirectory(prefix="sm64ar-pack-tool-test-")
        self.tmp = pathlib.Path(self._tmp.name)
        self.res = self.tmp / "res"
        self.res.mkdir()
        self.png = self.tmp / "png"
        self.png.mkdir()
        self.out = self.tmp / "out"

    def tearDown(self):
        self._tmp.cleanup()

    def run_tool(self, rom, *extra):
        return subprocess.run([str(TOOL), "--rom", str(rom), "--res", str(self.res), *extra],
                              capture_output=True, text=True, timeout=120)

    def assert_refused(self, r):
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn("error:", r.stderr)
        self.assertFalse(self.out.exists(), "the tool created OUT on a refused run")

    def test_missing_rom(self):
        (self.png / "a.png").write_bytes(b"\x89PNG\r\n\x1a\n")
        r = self.run_tool(self.tmp / "nope.z64", "--generate-tex", str(self.png), str(self.out))
        self.assert_refused(r)
        self.assertIn("no ROM", r.stderr)

    def test_missing_rom_is_checked_before_png_dir(self):
        # Empty PNG dir and no ROM: the ROM check comes first.
        r = self.run_tool(self.tmp / "nope.z64", "--generate-tex", str(self.png), str(self.out))
        self.assert_refused(r)
        self.assertIn("no ROM", r.stderr)
        self.assertNotIn("holding any .png", r.stderr)

    def test_empty_png_dir(self):
        # The PNG dir is checked before the ROM is loaded, so any existing file serves as the ROM here.
        rom = self.tmp / "small.z64"
        rom.write_bytes(bytes(16))
        r = self.run_tool(rom, "--generate-tex", str(self.png), str(self.out))
        self.assert_refused(r)
        self.assertIn("holding any .png", r.stderr)

    def test_missing_png_dir(self):
        rom = self.tmp / "small.z64"
        rom.write_bytes(bytes(16))
        r = self.run_tool(rom, "--generate-tex", str(self.tmp / "nothing"), str(self.out))
        self.assert_refused(r)
        self.assertIn("holding any .png", r.stderr)

    def test_wrong_size_rom(self):
        rom = self.tmp / "small.z64"
        rom.write_bytes(bytes(16))
        (self.png / "a.png").write_bytes(b"\x89PNG\r\n\x1a\n")
        r = self.run_tool(rom, "--generate-tex", str(self.png), str(self.out))
        self.assert_refused(r)

    def test_wrong_size_rom_actors(self):
        rom = self.tmp / "small.z64"
        rom.write_bytes(bytes(16))
        r = self.run_tool(rom, "--generate-actors", str(self.out))
        self.assert_refused(r)


if __name__ == "__main__":
    unittest.main()
