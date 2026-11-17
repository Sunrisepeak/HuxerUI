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


if __name__ == "__main__":
    unittest.main()
