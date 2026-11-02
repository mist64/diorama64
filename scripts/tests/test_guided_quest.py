import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import guided_build as gb  # noqa: E402


class FakeAsker:
    def __init__(self, answers=()):
        self.interactive, self.answers, self.asked, self.lines = True, list(answers), [], []

    def write(self, *t):
        self.lines.append(" ".join(map(str, t)))

    def optional(self, question):
        self.asked.append(question)
        return self.answers.pop(0)

    def text(self, question, flag, default=None):
        self.asked.append(question)
        return self.answers.pop(0)

    choose = text


def args(*argv):
    return gb.parse_args(list(argv))


class ResolveLinuxTests(unittest.TestCase):
    def test_quest_target_all_packs_no_apple_questions(self):
        asker = FakeAsker(["all"])
        c = gb.resolve(args(), {}, asker, platform="linux")
        self.assertEqual(c["target"], "quest")
        self.assertEqual(c["packs"], {pid: gb.OFFICIAL for pid in gb.known_packs()})
        self.assertEqual(asker.asked, [gb.PACKS_QUESTION])
        self.assertNotIn("team", c)

    def test_enter_means_all(self):
        c = gb.resolve(args(), {}, FakeAsker([""]), platform="linux")
        self.assertEqual(set(c["packs"]), set(gb.known_packs()))

    def test_saved_answers_are_not_asked_again(self):
        asker = FakeAsker()
        c = gb.resolve(args(), {"target": "quest", "packs": {}}, asker, platform="linux")
        self.assertEqual((c["target"], c["packs"], asker.asked), ("quest", {}, []))

    def test_apple_targets_refused(self):
        for t in ("headset", "simulator"):
            with self.assertRaisesRegex(gb.Stop, "builds for Meta Quest"):
                gb.resolve(args("--target", t), {}, FakeAsker(["all"]), platform="linux")

    def test_apple_flags_refused(self):
        for f in (("--team", "ABCDE12345"), ("--bundle-id", "a.b.c"), ("--device", "x"), ("--reinstall",)):
            with self.assertRaises(gb.Stop, msg=f):
                gb.resolve(args(*f), {}, FakeAsker(["all"]), platform="linux")


class MainLinuxTests(unittest.TestCase):
    def run_main(self, *argv):
        with mock.patch.object(gb.sys, "platform", "linux"):
            return gb.main(list(argv), FakeAsker())

    def test_missing_tools_named(self):
        with mock.patch.object(gb.common, "tool_problems", return_value=["magick"]):
            with self.assertRaisesRegex(gb.Stop, "magick"):
                self.run_main()

    def test_team_flag_refused(self):
        with mock.patch.object(gb.common, "tool_problems", return_value=[]):
            with self.assertRaises(gb.Stop):
                self.run_main("--team", "ABCDE12345")

    def test_no_device_stops(self):
        done = mock.Mock(returncode=0, stdout="List of devices attached\n\n")
        with mock.patch.object(gb.common, "tool_problems", return_value=[]), \
                mock.patch.object(gb, "git_checkout", return_value=True), \
                mock.patch.object(gb, "ensure_submodule"), mock.patch.object(gb, "us_sha1", return_value="x"), \
                mock.patch.object(gb, "rom_ok", return_value=True), \
                mock.patch.object(gb, "load_config", return_value={"target": "quest", "packs": {}}), \
                mock.patch.object(gb, "save_config"), mock.patch.object(gb.subprocess, "run", return_value=done):
            with self.assertRaisesRegex(gb.Stop, "connect one Quest"):
                self.run_main()


if __name__ == "__main__":
    unittest.main()
