"""Regression tests for the mcpp-side tooling.

Both scripts under test derive one artifact from another -- the module shell
and the scope prelude from the public headers, the drift verdict from the CMake
build -- so a defect in either is silent: it produces a plausible answer that is
wrong. Every case below is a defect that actually occurred while the mcpp build
was being written.
"""

import importlib.util
from pathlib import Path
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[2]


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, SOURCE / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


exports = load("gen_module_exports", "scripts/gen_module_exports.py")
parity = load("check_parity", "mcpp/parity/check_parity.py")


class ScannerTests(unittest.TestCase):
    """scripts/gen_module_exports.py finds the public names to re-export."""

    def scan(self, text):
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "sample.h"
            header.write_text(text)
            return exports.scan(header)

    def test_finds_namespace_scope_declarations(self):
        names = self.scan(
            "namespace huxerui {\n"
            "class Column final : public Layout<Column> {\n"
            "  void Member();\n"
            "};\n"
            "enum class TextRole { Title };\n"
            "using Views = std::vector<View>;\n"
            "View Divider();\n"
            "}\n"
        )
        self.assertEqual({"Column", "TextRole", "Views", "Divider"}, names)

    def test_ignores_members_and_the_detail_namespace(self):
        names = self.scan(
            "namespace huxerui {\n"
            "class Widget { public: void Hidden(); };\n"
            "namespace detail {\n"
            "class Internal {};\n"
            "}\n"
            "}\n"
        )
        self.assertEqual({"Widget"}, names)

    def test_a_brace_in_a_literal_does_not_close_the_namespace(self):
        # The defect: a single '}' in a char literal closed `namespace huxerui`
        # early, and every declaration after it was dropped -- view.h yielded 15
        # names instead of hundreds.
        names = self.scan(
            "namespace huxerui {\n"
            "inline char Closing() { return '}'; }\n"
            "class Later {};\n"
            "}\n"
        )
        self.assertIn("Later", names)

    def test_a_multi_line_macro_body_does_not_close_the_namespace(self):
        # The same failure from the other direction: a #define whose body has an
        # unbalanced '{' is skipped by its leading '#', but its continuation
        # lines are not.
        names = self.scan(
            "namespace huxerui {\n"
            "#define SCOPE_BEGIN \\\n"
            "  return Scope([=]() -> View {\n"
            "class Later {};\n"
            "}\n"
        )
        self.assertIn("Later", names)

    def test_a_single_line_template_declaration_is_found(self):
        names = self.scan(
            "namespace huxerui {\n"
            "template <class T> class State final : public Cell {};\n"
            "}\n"
        )
        self.assertIn("State", names)

    def test_an_out_of_class_member_definition_is_not_a_namespace_name(self):
        # `template <...> StringVariant StringVariant::Format(...)` sits at
        # namespace scope but `Format` does not exist in namespace huxerui, so
        # exporting it stopped the module compiling.
        names = self.scan(
            "namespace huxerui {\n"
            "class StringVariant {};\n"
            "template <class... A> StringVariant StringVariant::Format(A&&... a) { return {}; }\n"
            "}\n"
        )
        self.assertNotIn("Format", names)


class ScopeMacroTests(unittest.TestCase):
    """The prelude is lifted from view.h so the two copies cannot diverge."""

    def test_extracts_every_scope_macro_verbatim(self):
        macros = exports.extract_scope_macros()
        for name in exports.SCOPE_MACROS:
            self.assertIn(f"#define {name}", macros)
        # Continuation lines have to come along, or the prelude defines an
        # empty macro and hand-written composables silently produce nothing.
        self.assertIn("::huxerui::Scope(", macros)

    def test_the_committed_prelude_matches_the_header(self):
        self.assertEqual(exports.render_prelude(exports.extract_scope_macros()),
                         exports.PRELUDE.read_text())


class CMakeListTests(unittest.TestCase):
    """mcpp/parity/check_parity.py reads the CMake side it compares against."""

    def test_a_quoted_token_stays_one_token(self):
        # `"-framework AppKit"` is ONE token. Splitting on whitespace made every
        # framework name look unmatched and the parity check fail on macOS.
        tokens = parity.cmake_list(
            'set(HUXERUI_PLATFORM_LINK_LIBRARIES\n'
            '        "-framework AppKit"\n'
            '        "-weak_framework UniformTypeIdentifiers"\n'
            ')\n',
            "HUXERUI_PLATFORM_LINK_LIBRARIES")
        self.assertEqual(["-framework AppKit", "-weak_framework UniformTypeIdentifiers"], tokens)

    def test_bare_tokens_and_comments(self):
        tokens = parity.cmake_list(
            'set(HUXERUI_PLATFORM_LINK_LIBRARIES\n'
            '        advapi32  # the registry\n'
            '        d2d1\n'
            ')\n',
            "HUXERUI_PLATFORM_LINK_LIBRARIES")
        self.assertEqual(["advapi32", "d2d1"], tokens)

    def test_a_missing_variable_is_empty_rather_than_an_error(self):
        self.assertEqual([], parity.cmake_list("set(OTHER a)\n", "HUXERUI_PLATFORM_LINK_LIBRARIES"))


class GlobTests(unittest.TestCase):
    """Exclusion globs decide what the mcpp build compiles."""

    def test_exclusion_removes_a_match(self):
        # windows_installer.cpp is the WiX custom-action DLL, not part of the
        # framework; a bare `platform/windows/*.cpp` swept it into libhuxerui.
        included = parity.expand(["platform/windows/*.cpp"])
        excluded = parity.expand(
            ["platform/windows/*.cpp", "!platform/windows/windows_installer.cpp"])
        self.assertIn("platform/windows/windows_installer.cpp", included)
        self.assertNotIn("platform/windows/windows_installer.cpp", excluded)
        self.assertTrue(excluded < included)


class ParityRunTests(unittest.TestCase):
    """The checker must actually pass on this repository."""

    def test_the_repository_is_consistent(self):
        self.assertEqual(0, parity.main())


if __name__ == "__main__":
    unittest.main()
