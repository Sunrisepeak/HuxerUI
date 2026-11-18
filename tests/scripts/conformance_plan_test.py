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


def rows(paths, key="rows"):
    """The selected (target, host) rows of one conformance job."""
    return sorted((row["target"], row["host"]) for row in plan.json.loads(plan.outputs(paths)[key]))


class RowTests(unittest.TestCase):
    def test_nothing_selected_runs_every_row(self):
        self.assertEqual(len(rows([])), len(plan.ROWS))
        self.assertEqual(len(rows([], "macos_rows")), len(plan.MACOS_ROWS))

    def test_source_runs_each_target_on_its_primary_host(self):
        for path in ("src/runtime/ui_window.cpp", "include/huxerui/view.h", "templates/app/src/app.cppm.in",
                     "buildtools/cts/projects/app_minimal/expected.json"):
            with self.subTest(path=path):
                self.assertEqual(rows([path]), [("android", "linux"), ("linux", "linux"), ("web", "linux"),
                                                ("windows", "windows")])
                self.assertEqual(rows([path], "macos_rows"), [("ios", "macos"), ("macos", "macos")])

    def test_a_target_path_runs_that_target_on_every_host(self):
        for path in ("platform/android/huxerui/src/main/cpp/jni.cpp", "tools/huxerui_cli/platform/android.cpp",
                     "tools/huxerui_cli/templates/platform/android/app/build.gradle",
                     "buildtools/cts/projects/app_minimal/platform/android/AndroidManifest.xml",
                     "cmake/platform/Android.cmake"):
            with self.subTest(path=path):
                self.assertEqual(rows([path]), [("android", "linux"), ("android", "windows")])
                self.assertEqual(rows([path], "macos_rows"), [("android", "macos")])

    def test_ios_owns_its_cmake_platform_file(self):
        self.assertEqual(rows(["cmake/platform/IOS.cmake"]), [])
        self.assertEqual(rows(["cmake/platform/IOS.cmake"], "macos_rows"), [("ios", "macos")])

    def test_tooling_runs_every_row(self):
        for path in ("tools/huxerui_cli/cli.cpp", "buildtools/cts/cts.py", "buildtools/mcpp/support.json",
                     "cmake/HuxerUIApp.cmake", "mcpp.toml", "CMakeLists.txt",
                     ".github/workflows/build-system-conformance.yml"):
            with self.subTest(path=path):
                self.assertEqual(len(rows([path])), len(plan.ROWS))
                self.assertEqual(len(rows([path], "macos_rows")), len(plan.MACOS_ROWS))

    def test_a_target_path_beside_source_adds_its_other_hosts(self):
        self.assertIn(("android", "windows"), rows(["src/io/file.cpp", "platform/android/build.gradle"]))
        self.assertIn(("web", "linux"), rows(["src/io/file.cpp", "platform/android/build.gradle"]))
        self.assertNotIn(("web", "windows"), rows(["src/io/file.cpp", "platform/android/build.gradle"]))

    def test_jobs_without_rows_do_not_run(self):
        outputs = plan.outputs(["platform/ios/huxerui/app_delegate.mm"])
        self.assertEqual(outputs["rows"], "[]")
        self.assertEqual(outputs["compare"], "false")
        self.assertEqual(outputs["compare_macos"], "true")
        self.assertEqual(outputs["sequence"], "false")

    def test_one_integration_compares_nothing(self):
        outputs = plan.outputs(["mcpp.toml"])
        self.assertEqual(outputs["compare"], "false")
        self.assertEqual(outputs["compare_macos"], "false")


if __name__ == "__main__":
    unittest.main()
