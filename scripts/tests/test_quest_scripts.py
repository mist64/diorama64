import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from quest_device import find_device  # noqa: E402
from quest_package import aapt2_link_cmd  # noqa: E402


class QuestScriptTests(unittest.TestCase):
    def test_find_device_one(self):
        self.assertEqual(find_device("List of devices attached\n2G0YC1ZF8M0ABC\tdevice\n\n"), "2G0YC1ZF8M0ABC")

    def test_find_device_none_or_many(self):
        self.assertIsNone(find_device("List of devices attached\n\n"))
        self.assertIsNone(find_device("List of devices attached\nA\tdevice\nB\tdevice\n"))

    def test_find_device_unauthorized(self):
        self.assertIsNone(find_device("List of devices attached\nA\tunauthorized\n"))

    def test_aapt2_link_cmd(self):
        cmd = aapt2_link_cmd("/t/aapt2", "/m/AndroidManifest.xml", "/s/android.jar", "/a", "/o/base.apk")
        self.assertEqual(cmd, ["/t/aapt2", "link", "-o", "/o/base.apk", "--manifest", "/m/AndroidManifest.xml",
                               "-I", "/s/android.jar", "--min-sdk-version", "32", "--target-sdk-version", "32",
                               "-A", "/a"])

    def test_aapt2_link_cmd_no_assets(self):
        self.assertNotIn("-A", aapt2_link_cmd("a", "m", "j", None, "o"))

    def test_missing_assets(self):
        from quest_package import missing_assets
        self.assertEqual(missing_assets(["a/.x", "b", "c/_d/e"], ["assets/b", "assets/c/_d/f"]), ["a/.x", "c/_d/e"])
        self.assertEqual(missing_assets([], []), [])

    def test_quest_bundle_assets_layout(self):
        import json, tempfile
        from unittest import mock
        import bundle_local, quest_bundle
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            res = t / "us_pc"
            for rel in ("lang/English.ini", "dynos/packs/a.bin", "palettes/p.pal", "mods/m/main.lua"):
                (res / rel).parent.mkdir(parents=True, exist_ok=True)
                (res / rel).write_text(rel)

            def fake_stage(dest, names=None, **kw):
                dest = Path(dest)
                (dest / "packs" / "pk").mkdir(parents=True)
                (dest / "rom.z64").write_bytes(b"rom")
                (dest / "packs" / "pk" / "t.png").write_bytes(b"png")
                return {"packs/pk/t.png": "bb", "rom.z64": "aa"}

            out = t / "assets"
            with mock.patch.object(bundle_local, "stage", fake_stage):
                expected = quest_bundle.stage(out, ["pk"], res_dir=res)
            self.assertEqual(expected, {"packs/pk/t.png": "bb", "rom.z64": "aa"})
            self.assertEqual(json.loads((out / "Bundled" / "bundled.json").read_text()), {"files": expected})
            listed = json.loads((out / "res" / "res.json").read_text())
            self.assertEqual(sorted(listed), ["dynos/packs/a.bin", "lang/English.ini", "mods/m/main.lua",
                                              "palettes/p.pal"])
            for rel in listed:
                self.assertTrue((out / "res" / rel).is_file())


if __name__ == "__main__":
    unittest.main()
