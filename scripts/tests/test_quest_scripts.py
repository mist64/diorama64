import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from quest_device import dev_data_plan, find_device  # noqa: E402
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

    def test_dev_data_plan(self):
        plan = dev_data_plan(Path("/l/rom.z64"), Path("/b/us_pc"), [Path("/p/a"), Path("/p/b")])
        self.assertEqual(plan[0], (Path("/l/rom.z64"), "/sdcard/Android/data/org.pagetable.diorama64/files/rom.z64"))
        self.assertIn((Path("/b/us_pc/lang"), "/sdcard/Android/data/org.pagetable.diorama64/files/res/lang"), plan)
        self.assertIn((Path("/p/b"), "/sdcard/Android/data/org.pagetable.diorama64/files/packs/b"), plan)
        self.assertEqual(len(plan), 1 + 4 + 2)


if __name__ == "__main__":
    unittest.main()
