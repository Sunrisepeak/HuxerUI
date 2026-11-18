"""Tests for the build-system conformance runner's discovery, record schema, and clause coverage."""

import argparse
import importlib.util
import sys
import tempfile
from pathlib import Path
import unittest
from unittest import mock


SOURCE = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("cts", SOURCE / "buildtools/cts/cts.py")
cts = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cts)
adapter_api = sys.modules["adapter_api"]


def record(**overrides):
    value = {"target": "app", "artifact": "/out/app", "profile": "debug", "libraries": []}
    value.update(overrides)
    return value


class DiscoveryTests(unittest.TestCase):
    def test_every_support_declaration_is_an_integration(self):
        declared = {path.parent.name for path in (SOURCE / "buildtools").glob("*/support.json")}
        self.assertEqual(set(cts.discover_integrations()), declared)

    def test_every_integration_ships_an_adapter(self):
        self.assertEqual(set(cts.ADAPTERS), set(cts.discover_integrations()))

    def test_adapter_options_carry_the_integration_name(self):
        parser = argparse.ArgumentParser()
        options = cts.PrefixedOptions(parser, "cmake")
        options.add_argument("--cmake-home")
        with self.assertRaises(SystemExit):
            options.add_argument("--home")

    def test_every_adapter_adds_only_prefixed_options(self):
        for name, module in cts.ADAPTERS.items():
            with self.subTest(integration=name):
                module.add_arguments(cts.PrefixedOptions(argparse.ArgumentParser(), name))


class RecordTests(unittest.TestCase):
    def test_optional_fields_are_filled_in(self):
        validated = adapter_api.validate_record("x", record())
        self.assertEqual(validated["bundle"], "")
        self.assertEqual(validated["launch_environment"], {})

    def test_missing_required_field_is_an_error(self):
        value = record()
        del value["profile"]
        with self.assertRaisesRegex(adapter_api.RecordError, "missing field profile"):
            adapter_api.validate_record("x", value)

    def test_unknown_field_is_an_error(self):
        with self.assertRaisesRegex(adapter_api.RecordError, "unknown field extra"):
            adapter_api.validate_record("x", record(extra=1))

    def test_library_entries_need_target_and_root(self):
        with self.assertRaisesRegex(adapter_api.RecordError, r"libraries\[0\]"):
            adapter_api.validate_record("x", record(libraries=[{"target": "A::B"}]))

    def test_record_error_fails_the_build(self):
        self.assertTrue(issubclass(adapter_api.RecordError, RuntimeError))


class RebuildTests(unittest.TestCase):
    def test_runner_builds_twice_and_records_the_rebuild(self):
        project = cts.PROJECTS / "app_minimal"
        adapter = mock.Mock()
        adapter.build.return_value = record(target="cts_app_minimal")
        with mock.patch.object(cts, "collect_facts", return_value={"profile": "debug"}), \
                mock.patch.object(cts, "target_support", return_value=({"capabilities": []}, "")):
            with tempfile.TemporaryDirectory() as out:
                result = cts.build_project("x", adapter, "linux", "linux", project, Path(out))
        self.assertEqual(adapter.build.call_count, 2)
        self.assertEqual(result["status"], "built")
        self.assertEqual(result["facts"]["rebuild"], "succeeded")

    def test_failed_second_build_fails_the_project(self):
        project = cts.PROJECTS / "app_minimal"
        adapter = mock.Mock()
        adapter.build.side_effect = [record(), RuntimeError("boom")]
        with mock.patch.object(cts, "target_support", return_value=({"capabilities": []}, "")):
            with tempfile.TemporaryDirectory() as out:
                result = cts.build_project("x", adapter, "linux", "linux", project, Path(out))
        self.assertEqual(result["status"], "failed")
        self.assertIn("second build of unchanged inputs", result["reason"])


class CreatedProjectTests(unittest.TestCase):
    def setUp(self):
        self.suite = cts.load_json(cts.CREATED_PROJECTS)

    def test_every_created_project_has_expected_facts(self):
        for name, project in self.suite["projects"].items():
            expected = cts.created_expected(name, project, self.suite)
            self.assertEqual(expected["project"], name)
            self.assertNotRegex(cts.json.dumps(expected), r"\{(name|product)\}", name)

    def test_style_follows_the_build_systems(self):
        self.assertEqual(cts.created_style(["mcpp"]), "modules")
        self.assertEqual(cts.created_style(["cmake"]), "headers")
        self.assertEqual(cts.created_style(["cmake", "mcpp"]), "headers")

    def test_a_second_declaration_does_not_change_the_expected_facts(self):
        projects = self.suite["projects"]
        single = cts.created_expected("x", projects["created_cmake_app"], self.suite)
        dual = cts.created_expected("x", projects["created_dual_app"], self.suite)
        self.assertEqual(single, dual)

    def test_library_preview_names_follow_the_library(self):
        expected = cts.created_expected("data-grid", {"kind": "library", "build-systems": ["mcpp"]}, self.suite)
        self.assertEqual(expected["target"], "example_data-grid")
        self.assertEqual(expected["facts"]["libraries"], ["DataGrid::DataGrid"])

    def test_created_projects_are_selected_with_their_names(self):
        with tempfile.TemporaryDirectory() as out:
            application = Path(out) / "somewhere" / "preview"
            application.mkdir(parents=True)
            cts.write_json(application / "expected.json", {"project": "created_x"})
            cts.write_json(cts.created_root(Path(out), "x", "linux") / "projects.json",
                           {"projects": [str(application)]})
            selected = cts.selected_projects(None, Path(out), "x", "linux")
            self.assertEqual(cts.project_name(selected[-1]), "created_x")
            self.assertEqual(cts.selected_projects(["created_x"], Path(out), "x", "linux"), [application])
            self.assertNotIn(application, cts.selected_projects(None, Path(out), "x", "web"))

    def test_undeclared_host_creates_nothing(self):
        arguments = argparse.Namespace(integration="cmake", target="web", host="no-such-host", out=None, project=None,
                                       cli=None)
        with tempfile.TemporaryDirectory() as out:
            arguments.out = out
            self.assertEqual(cts.command_create(arguments), 0)
            self.assertEqual(cts.selected_projects(None, Path(out), "cmake", "web"), cts.selected_projects(None))


class DispatchTests(unittest.TestCase):
    def selection(self, files, integration):
        with tempfile.TemporaryDirectory() as root:
            for name in files:
                (Path(root) / name).write_text("", encoding="utf-8")
            return cts.dispatch_selection(integration, Path(root))

    def test_a_root_declaring_one_build_system_is_dispatched_by_its_file(self):
        self.assertEqual(self.selection(["CMakeLists.txt"], "cmake"), [])
        self.assertEqual(self.selection(["mcpp.toml"], "mcpp"), [])

    def test_a_root_declaring_several_takes_the_default_unless_another_is_selected(self):
        self.assertEqual(self.selection(["CMakeLists.txt", "mcpp.toml"], "cmake"), [])
        self.assertEqual(self.selection(["CMakeLists.txt", "mcpp.toml"], "mcpp"), ["--build-system", "mcpp"])

    def test_every_integration_names_its_declaration(self):
        for name, directory in cts.discover_integrations().items():
            with self.subTest(integration=name):
                self.assertTrue(cts.load_json(directory / "support.json")["declaration"])

    def test_the_stated_integration_is_read_from_the_cli_output(self):
        self.assertEqual(cts.dispatched_integration("Building android (release, x86_64) with cmake\n> gradle"), "cmake")
        self.assertEqual(cts.dispatched_integration("Packaging linux (release, appimage) with mcpp\n"), "mcpp")
        self.assertIsNone(cts.dispatched_integration("Packaged dist/linux/app.AppImage\n"))


class CreatedFactTests(unittest.TestCase):
    def test_facts_observed_at_creation_join_the_build(self):
        adapter = mock.Mock()
        adapter.build.return_value = record(target="cts_app_minimal")
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory) / "created_x"
            project.mkdir()
            cts.write_json(project / "expected.json", {"project": "created_x", "facts": {}})
            cts.write_json(project / "created.json", {"facts": {"create.declarations": cts.CREATED_DECLARATIONS}})
            with mock.patch.object(cts, "collect_facts", return_value={"profile": "debug"}), \
                    mock.patch.object(cts, "target_support", return_value=({"capabilities": []}, "")):
                result = cts.build_project("x", adapter, "linux", "linux", project, Path(directory) / "out")
        self.assertEqual(result["facts"]["create.declarations"], cts.CREATED_DECLARATIONS)


class SequenceTests(unittest.TestCase):
    def run_sequence(self, facts):
        """Runs `sequence` over linux,web for app_minimal with an adapter whose builds record `facts` in turn."""
        adapter = mock.Mock()
        adapter.build.return_value = record(target="cts_app_minimal")
        module = mock.Mock()
        module.Adapter.return_value = adapter
        with tempfile.TemporaryDirectory() as out, \
                mock.patch.dict(cts.ADAPTERS, {"x": module}), \
                mock.patch.object(cts, "collect_facts", side_effect=facts), \
                mock.patch.object(cts, "target_support", return_value=({"capabilities": []}, "")), \
                mock.patch.object(cts, "check_project", return_value=("pass", "")):
            arguments = argparse.Namespace(integration="x", targets="linux,web", host="linux", out=out,
                                           project=["app_minimal"], cli=None)
            return cts.command_sequence(arguments), adapter.build.call_args_list

    def test_every_target_and_the_rebuild_of_the_first_are_built(self):
        status, calls = self.run_sequence([{"a": 1}, {"a": 2}, {"a": 1}])
        self.assertEqual(status, 0)
        # Each build_project builds twice: linux, web, then linux again.
        self.assertEqual([call.args[2] for call in calls], ["linux", "linux", "web", "web", "linux", "linux"])

    def test_a_rebuild_that_records_other_facts_fails(self):
        status, _ = self.run_sequence([{"a": 1}, {"a": 2}, {"a": 3}])
        self.assertEqual(status, 1)


class DeviceLaunchTests(unittest.TestCase):
    def test_probe_lines_are_read_with_and_without_the_prefix(self):
        text = "HUXERUI_CTS_PROBE app.title=CTS app\napp.greeting=Hello = there\nsome other line\n"
        self.assertEqual(cts.probe_lines(text), {"app.title": "CTS app", "app.greeting": "Hello = there"})

    def test_nothing_recorded_is_unavailable(self):
        with tempfile.TemporaryDirectory() as work:
            with self.assertRaises(cts.Unavailable):
                cts.device_launch_result({}, {"app.title": "x"}, Path(work) / "launch.log", "device")

    def test_a_missing_fact_is_reported(self):
        facts = cts.device_launch_result({"a": "1"}, {"a": "1", "b": "2"}, Path("unused"), "device")
        self.assertEqual(facts, {"launch.a": "1", "launch.b": "<missing>"})

    def test_launch_without_a_published_package_is_unavailable(self):
        with tempfile.TemporaryDirectory() as work:
            with self.assertRaisesRegex(cts.Unavailable, "package checkpoint"):
                cts.android_launch_facts({"a": "1"}, Path(work), 1.0)
            with self.assertRaisesRegex(cts.Unavailable, "package checkpoint"):
                cts.ios_launch_facts({"a": "1"}, Path(work), 1.0)


class CoverageTests(unittest.TestCase):
    def test_contract_has_clauses(self):
        clauses = cts.contract_clauses()
        self.assertIn("LINK-STATIC", clauses)
        self.assertIn("PKG-KIND", clauses)

    def test_checkpoints_name_only_known_clauses(self):
        _, problems = cts.coverage_rows()
        self.assertEqual(problems, [])

    def test_unknown_clause_is_reported(self):
        checks = {"build": {"title": "", "facts": {"profile": ["NO-SUCH-CLAUSE"]}, "equivalent": {}}}
        with mock.patch.object(cts, "CHECKS", checks):
            _, problems = cts.coverage_rows()
        self.assertTrue(any("NO-SUCH-CLAUSE" in problem for problem in problems))

    def test_compared_facts_do_not_count_as_checked(self):
        rows = {clause: state for clause, state, _ in cts.coverage_rows()[0]}
        self.assertEqual(rows["RES-PACKAGE"], "compared only")
        self.assertEqual(rows["RES-LOCATION"], "checked")

    def test_readme_table_is_current(self):
        self.assertEqual(cts.command_coverage(argparse.Namespace(check=True, write=False)), 0)


if __name__ == "__main__":
    unittest.main()
