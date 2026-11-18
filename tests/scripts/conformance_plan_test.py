"""Tests for the conformance workflow's selection of integrations by path ownership."""

import importlib.util
from pathlib import Path
import unittest


SOURCE = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("conformance_plan",
                                              SOURCE / ".github/workflows/scripts/conformance_plan.py")
plan = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plan)


class SelectionTests(unittest.TestCase):
    def test_owned_paths_select_their_integration(self):
        self.assertEqual(plan.select(["cmake/HuxerUIApp.cmake", "CMakeLists.txt"]), ["cmake"])
        self.assertEqual(plan.select(["buildtools/cmake/support.json"]), ["cmake"])
        self.assertEqual(plan.select(["buildtools/cts/projects/app_library/CMakeLists.txt"]), ["cmake"])
        self.assertEqual(plan.select(["buildtools/mcpp/huxerui-build-rules/src/rules.cppm"]), ["mcpp"])
        self.assertEqual(plan.select(["mcpp.toml", "build.mcpp", "modules/huxerui.cppm"]), ["mcpp"])
        self.assertEqual(plan.select(["templates/app/src/app.cppm.in"]), ["mcpp"])
        self.assertEqual(plan.select(["buildtools/cts/projects/app_modules/src/app.cppm"]), ["mcpp"])

    def test_shared_paths_select_every_integration(self):
        for path in ("src/runtime/ui_window.cpp", "include/huxerui/view.h", "buildtools/cts/cts.py",
                     "buildtools/cts/projects/app_minimal/expected.json", "platform/android/build.gradle"):
            with self.subTest(path=path):
                self.assertEqual(plan.select([path]), ["cmake", "mcpp"])

    def test_tool_and_workflow_paths_are_shared_whatever_their_name(self):
        self.assertEqual(plan.select(["tools/huxerui_cli/CMakeLists.txt"]), ["cmake", "mcpp"])
        self.assertEqual(plan.select([".github/workflows/build-system-conformance.yml"]), ["cmake", "mcpp"])

    def test_a_change_owned_by_both_selects_both(self):
        self.assertEqual(plan.select(["mcpp.toml", "CMakeLists.txt"]), ["cmake", "mcpp"])

    def test_a_shared_path_wins_over_owned_ones(self):
        self.assertEqual(plan.select(["mcpp.toml", "src/io/file.cpp"]), ["cmake", "mcpp"])

    def test_nothing_selected_runs_everything(self):
        self.assertEqual(plan.select(["", "  "]), ["cmake", "mcpp"])


if __name__ == "__main__":
    unittest.main()
