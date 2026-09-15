// mcpp's contract is the exit code: this program passes by returning 0.

import std;
import huxerui.rules.sources;

namespace {

int failures = 0;

void check(bool condition, std::string_view what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

using huxerui::rules::sources::matches;
using huxerui::rules::sources::needs_codegen;
using huxerui::rules::sources::identifier;
using huxerui::rules::sources::cmake_list;
using huxerui::rules::sources::macro_definition;
using huxerui::rules::sources::public_names;
using huxerui::rules::sources::scan_module_interface;
using huxerui::rules::sources::strip_noncode;
using huxerui::rules::sources::umbrella_includes;
using huxerui::rules::sources::without_entry;
using huxerui::rules::sources::library_resource_namespace;
using huxerui::rules::sources::read_dependency_manifest;
using huxerui::rules::sources::dependency_keys;

void glob_single_star_stays_within_one_segment() {
    check(matches("src/*/*.cpp", "src/runtime/view.cpp"), "one directory matches src/*/*.cpp");
    // The defect this exists for: `*` must NOT cross a separator, or
    // `src/*/*.cpp` selects the same set as `src/**/*.cpp` and the manifest's
    // two spellings stop meaning different things.
    check(!matches("src/*/*.cpp", "src/runtime/detail/view.cpp"),
          "two directories do not match src/*/*.cpp");
    check(!matches("src/*.cpp", "src/runtime/view.cpp"), "src/*.cpp is one segment");
}

void glob_double_star_crosses_separators() {
    check(matches("src/**/*.cpp", "src/runtime/view.cpp"), "one directory matches src/**/*.cpp");
    check(matches("src/**/*.cpp", "src/a/b/c/view.cpp"), "three directories match src/**/*.cpp");
    // `**/` matches zero directories too, which is what makes a single glob
    // cover a flat project and a nested one.
    check(matches("src/**/*.cpp", "src/main.cpp"), "zero directories match src/**/*.cpp");
}

void glob_anchors_at_both_ends() {
    check(!matches("src/*.cpp", "other/src/main.cpp"), "pattern is anchored at the start");
    check(!matches("src/*.cpp", "src/main.cpp.bak"), "pattern is anchored at the end");
    check(matches("platform/windows/*.cpp", "platform/windows/win32_adapter.cpp"),
          "platform glob matches");
    check(!matches("platform/windows/*.cpp", "platform/linux/linux_adapter.cpp"),
          "platform glob rejects another platform");
}

void codegen_prefilter_matches_the_cmake_rule() {
    check(needs_codegen("[[huxerui::composable]]\nView Counter() {}"), "marker is detected");
    check(needs_codegen("auto value = UseState(0);"), "Use call is detected");
    // Anything else must produce NO transform edge; the cost of a false
    // positive is a needless build-graph node per source.
    check(!needs_codegen("#include <huxerui/huxerui.h>\nint main() { return 0; }"),
          "plain source needs no transform");
}

void entry_is_excluded_from_the_transform_set() {
    const std::vector<std::string> sources{"src/main.cpp", "src/counter.cpp"};
    const std::vector<std::string> kept = without_entry(sources, "src/main.cpp");
    check(kept.size() == 1 && kept.front() == "src/counter.cpp", "entry is removed");
    check(without_entry(sources, "src/absent.cpp").size() == 2, "an absent entry removes nothing");
}

void package_names_become_identifiers() {
    check(identifier("demo") == "demo", "a plain name is unchanged");
    // The defect this exists for: hrc refuses `mod-proj` with
    // "resource namespace must be a non-reserved C++ identifier".
    check(identifier("mod-proj") == "mod_proj", "a hyphen becomes an underscore");
    check(identifier("my.app") == "my_app", "a dot becomes an underscore");
    check(identifier("2fast") == "_2fast", "a leading digit is prefixed");
    check(identifier("") == "resources", "an empty name still yields an identifier");
}

void module_interface_is_read_from_the_source() {
    const auto m = scan_module_interface(
        "module;\n"
        "#include <typeinfo>\n"
        "export module app;\n"
        "import huxerui;\n"
        "import std;\n");
    // Without these two the importer compiles before the generated interface
    // exists and fails with `failed to read compiled module: app`.
    check(m.name == "app", "the declared module name is read");
    check(m.imports.size() == 2 && m.imports[0] == "huxerui" && m.imports[1] == "std",
          "every import is read");

    const auto none = scan_module_interface("#include <x>\nint main() { return 0; }\n");
    check(none.name.empty(), "an ordinary translation unit declares no module");

    // A commented-out declaration is not one.
    const auto commented = scan_module_interface("// export module ghost;\nexport module real;\n");
    check(commented.name == "real", "a comment is not a declaration");
}

bool has(const std::vector<std::string>& names, std::string_view what) {
    return std::ranges::find(names, what) != names.end();
}

void header_scan_finds_namespace_scope_declarations() {
    const auto names = public_names(
        "namespace huxerui {\n"
        "class Column final : public Layout<Column> {\n"
        "  void Member();\n"
        "};\n"
        "enum class TextRole { Title };\n"
        "using Views = std::vector<View>;\n"
        "View Divider();\n"
        "}\n");
    check(has(names, "Column"), "a class is found");
    check(has(names, "TextRole"), "an enum is found");
    check(has(names, "Views"), "a using alias is found");
    check(has(names, "Divider"), "a function is found");
    check(!has(names, "Member"), "a member function is not a namespace-scope name");
}

void header_scan_sees_past_an_attribute() {
    // `class [[nodiscard]] Result final {` -- the attribute sits between the
    // keyword and the name, and skipping it is what makes Result reachable
    // through `import huxerui;`.
    const auto names = public_names(
        "namespace huxerui {\n"
        "template <class T, class E> class [[nodiscard]] Result final {\n"
        "};\n"
        "}\n");
    check(has(names, "Result"), "an attributed class is found");
}

void header_scan_ignores_the_detail_namespace() {
    const auto names = public_names(
        "namespace huxerui {\n"
        "class Widget {};\n"
        "namespace detail {\n"
        "class Internal {};\n"
        "}\n"
        "}\n");
    check(has(names, "Widget"), "the public class is found");
    check(!has(names, "Internal"), "detail is excluded");
}

void header_scan_survives_braces_in_literals_and_macros() {
    // A '}' in a char literal closed the namespace early: view.h yielded 15
    // names instead of hundreds.
    const auto literal = public_names(
        "namespace huxerui {\n"
        "inline char Closing() { return '}'; }\n"
        "class Later {};\n"
        "}\n");
    check(has(literal, "Later"), "a brace in a char literal is not scope");

    // The same from the other direction: a #define is skipped by its leading
    // '#', but its CONTINUATION lines are not.
    const auto macro = public_names(
        "namespace huxerui {\n"
        "#define SCOPE_BEGIN \\\n"
        "  return Scope([=]() -> View {\n"
        "class Later {};\n"
        "}\n");
    check(has(macro, "Later"), "an unbalanced brace in a macro body is not scope");
}

void header_scan_handles_templates_and_qualified_names() {
    const auto single_line = public_names(
        "namespace huxerui {\n"
        "template <class T> class State final : public Cell {};\n"
        "}\n");
    check(has(single_line, "State"), "a single-line template declaration is found");

    // `StringVariant StringVariant::Format(...)` is at namespace scope but
    // `Format` does not exist in namespace huxerui; exporting it stopped the
    // module compiling.
    const auto qualified = public_names(
        "namespace huxerui {\n"
        "class StringVariant {};\n"
        "template <class... A> StringVariant StringVariant::Format(A&&... a) { return {}; }\n"
        "}\n");
    check(!has(qualified, "Format"), "an out-of-class definition is not a namespace name");
}

void strip_noncode_removes_what_it_should() {
    const std::string out = strip_noncode("int a; // }\n/* } */ int b;\n\"}\" '}'\n");
    check(out.find('}') == std::string::npos, "no brace survives a comment or literal");
    check(out.find("int a;") != std::string::npos, "code survives");
    check(std::ranges::count(out, '\n') == 3, "line structure survives");
}

void cmake_lists_keep_quoted_tokens_whole() {
    // `"-framework AppKit"` is ONE token; splitting on whitespace made every
    // macOS framework look unmatched.
    const auto frameworks = cmake_list(
        "set(HUXERUI_PLATFORM_LINK_LIBRARIES\n"
        "        \"-framework AppKit\"\n"
        "        \"-weak_framework UniformTypeIdentifiers\"\n"
        ")\n",
        "HUXERUI_PLATFORM_LINK_LIBRARIES");
    check(frameworks.size() == 2 && frameworks[0] == "-framework AppKit",
          "a quoted token stays whole");

    const auto libraries = cmake_list(
        "set(HUXERUI_PLATFORM_LINK_LIBRARIES\n"
        "        advapi32  # the registry\n"
        "        d2d1\n"
        ")\n",
        "HUXERUI_PLATFORM_LINK_LIBRARIES");
    check(libraries.size() == 2 && libraries[0] == "advapi32" && libraries[1] == "d2d1",
          "bare tokens and comments");

    check(cmake_list("set(OTHER a)\n", "HUXERUI_PLATFORM_LINK_LIBRARIES").empty(),
          "a missing variable is empty rather than an error");
}

void macro_definitions_come_out_verbatim() {
    const std::string header =
        "#define HUXERUI_SCOPE(...) return ::huxerui::Scope(__VA_ARGS__)\n"
        "\n"
        "#define HUXERUI_SCOPE_BEGIN \\\n"
        "  return ::huxerui::Scope([=]() -> ::huxerui::View {\n"
        "int after;\n";
    // A prefix must not match a longer name, or HUXERUI_SCOPE picks up
    // HUXERUI_SCOPE_BEGIN and the prelude defines the wrong thing.
    const std::string scope = macro_definition(header, "HUXERUI_SCOPE");
    check(scope.find("__VA_ARGS__") != std::string::npos, "HUXERUI_SCOPE is the short one");
    check(scope.find("SCOPE_BEGIN") == std::string::npos, "it did not swallow the longer name");

    // Continuations have to come along, or the prelude defines an empty macro
    // and hand-written composables silently produce nothing.
    const std::string begin = macro_definition(header, "HUXERUI_SCOPE_BEGIN");
    check(begin.find("::huxerui::Scope([=]()") != std::string::npos, "the continuation is included");
    check(begin.find("int after") == std::string::npos, "it stops at the end of the definition");

    check(macro_definition(header, "HUXERUI_ABSENT").empty(), "a missing macro is empty");
}

void umbrella_includes_are_read_in_order() {
    const auto list = umbrella_includes(
        "#pragma once\n#include <huxerui/view.h>\n#include <string>\n#include <huxerui/app.h>\n");
    check(list.size() == 2 && list[0] == "view.h" && list[1] == "app.h",
          "only huxerui headers, in order");
}

} // namespace

// ------------------------------------------------------------- installer --

void resource_outputs_predict_what_hrc_writes() {
    using huxerui::rules::sources::resource_outputs;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "huxerui-resource-outputs-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "images" / "nested");
    std::filesystem::create_directories(root / "raw" / "models");
    std::filesystem::create_directories(root / "strings");
    for (const char* f : { "images/check.svg", "images/logo.png", "images/nested/photo.JPG",
                           "images/notes.txt", "raw/models/a.moc3", "strings/default.properties" }) {
        std::ofstream(root / f) << "x";
    }
    const auto out = resource_outputs(root, "app");
    const auto has = [&](std::string_view p) { return std::ranges::find(out, p) != out.end(); };
    // hrc compiles an SVG to .huxv and keeps every other image's name.
    check(has("huxerui/app/images/check.huxv"), "an svg becomes a .huxv");
    check(has("huxerui/app/images/logo.png"), "a png keeps its name");
    check(has("huxerui/app/images/nested/photo.JPG"), "a nested raster keeps its path and case");
    check(!has("huxerui/app/images/notes.txt"), "a non-image under images/ is not a payload");
    check(has("huxerui/app/raw/models/a.moc3"), "a raw file keeps its path");
    // Strings are index entries, not files; the index itself is always last.
    check(!std::ranges::any_of(out, [](const std::string& p) { return p.find("/strings/") != std::string::npos; }),
          "strings produce no payload");
    check(out.back() == "huxerui/resources.bin", "the index is the last output");
    check(out.size() == 5, "exactly the four payloads and the index");
    std::filesystem::remove_all(root);
}

void sha1_matches_the_standard_vectors() {
    using huxerui::rules::sources::sha1;
    const auto hex = [](const std::array<std::uint8_t, 20>& digest) {
        static constexpr std::string_view digits = "0123456789abcdef";
        std::string out;
        for (std::uint8_t b : digest) {
            out.push_back(digits[b >> 4U]);
            out.push_back(digits[b & 0x0FU]);
        }
        return out;
    };
    check(hex(sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1(abc)");
    check(hex(sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "sha1 of nothing");
    // Several blocks, and a length whose padding spills into a second one.
    check(hex(sha1(std::string(1000, 'a'))) == "291e9a6c66994949b57ba5e650361e98fc36b1ba", "sha1 over 1000 bytes");
}

// The values CMake's `string(UUID ... TYPE SHA1 UPPER)` returns, which are
// RFC 4122's version 5 (checked against Python's uuid.uuid5).
void upgrade_codes_are_the_ones_cmake_derives() {
    using huxerui::rules::sources::uuid_v5;
    using huxerui::rules::sources::windows_upgrade_code_namespace;
    check(uuid_v5(windows_upgrade_code_namespace, "python.org") == "886313E1-3B8A-5372-9B90-0C9AEE199E5D",
          "the RFC 4122 example name");
    check(uuid_v5(windows_upgrade_code_namespace, "com.example.sampleapp.msi") ==
              "999C638A-38BF-5DA7-97A5-A024AF215002",
          "a project's MSI upgrade code");
    check(uuid_v5(windows_upgrade_code_namespace, "com.example.sampleapp.bundle") ==
              "B7C082F6-45C1-5746-A31B-CBBE81916FB5",
          "a project's bundle upgrade code");
}

// The templates CMake configures, found from wherever `mcpp test` runs.
std::string cli_windows_template(std::string_view name) {
    for (std::filesystem::path at = std::filesystem::current_path(); !at.empty(); at = at.parent_path()) {
        const std::filesystem::path candidate =
            at / "tools/huxerui_cli/templates/platform/windows/app/package" / name;
        if (std::filesystem::is_regular_file(candidate)) {
            std::ifstream in(candidate, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        if (at == at.parent_path()) break;
    }
    return {};
}

huxerui::rules::sources::windows_installer_values installer_values() {
    huxerui::rules::sources::windows_installer_values v;
    v.project_name        = "Sample & App";
    v.project_id          = "com.example.sampleapp";
    v.version             = "0.1.0";
    v.target_name         = "SampleApp";
    v.msi_upgrade_code    = "999C638A-38BF-5DA7-97A5-A024AF215002";
    v.bundle_upgrade_code = "B7C082F6-45C1-5746-A31B-CBBE81916FB5";
    v.icon                = "C:/project/assets/app.ico";
    v.interface_program   = "C:/tools/SampleApp-Installer.exe";
    v.payloads            = {{"C:/wix/mbanative.dll", "mbanative.dll"},
                             {"C:/out/final/package/huxerui/resources.bin",
                              "SampleApp-Installer.resources/huxerui/resources.bin"}};
    return v;
}

// Rendered from the SAME files CMake configures: a template change that moves
// a line this rule replaces fails here, on Linux, before a Windows build.
void package_definition_is_cmakes_with_the_staged_files() {
    using huxerui::rules::sources::windows_package_definition;
    const std::string tmpl = cli_windows_template("Package.wxs.in");
    check(!tmpl.empty(), "Package.wxs.in is found from the test's directory");
    const auto r = windows_package_definition(tmpl, installer_values());
    check(r.error.empty(), "Package.wxs renders: " + r.error);
    const auto has = [&](std::string_view s) { return r.text.find(s) != std::string::npos; };
    check(has("<File Source=\"$(Executable)\" KeyPath=\"yes\" />"), "the program is $(Executable)");
    check(has("<ComponentGroupRef Id=\"StagedFiles\" />"), "the staged files are installed");
    check(!has("!(bindpath."), "no bind path remains");
    check(has("Name=\"Sample &amp; App\""), "the product name is escaped");
    check(has("UpgradeCode=\"999C638A-38BF-5DA7-97A5-A024AF215002\""), "the MSI upgrade code");
    check(has("Target=\"[INSTALLFOLDER]SampleApp.exe\""), "the shortcut names the program");
    check(has("SourceFile=\"C:/project/assets/app.ico\""), "the icon is named by path");
}

void bundle_definition_is_cmakes_with_the_interface_and_payloads() {
    using huxerui::rules::sources::windows_bundle_definition;
    const std::string tmpl = cli_windows_template("Bundle.wxs.in");
    check(!tmpl.empty(), "Bundle.wxs.in is found from the test's directory");
    const auto r = windows_bundle_definition(tmpl, installer_values());
    check(r.error.empty(), "Bundle.wxs renders: " + r.error);
    const auto has = [&](std::string_view s) { return r.text.find(s) != std::string::npos; };
    check(has("<BootstrapperApplication SourceFile=\"C:/tools/SampleApp-Installer.exe\">"),
          "the interface is the program mcpp built");
    check(has("<MsiPackage SourceFile=\"$(Msi)\">"), "the MSI is dist-wix's $(Msi)");
    check(has("<PayloadGroup Id=\"HuxerUIInstallerPayloads\">"), "the payload group CMake generates");
    check(has("Name=\"SampleApp-Installer.resources\\huxerui\\resources.bin\""),
          "a payload is named with Windows separators");
    check(has("UpgradeCode=\"B7C082F6-45C1-5746-A31B-CBBE81916FB5\""), "the bundle upgrade code");
    check(!has("!(bindpath."), "no bind path remains");
    check(r.text.rfind("</Wix>") != std::string::npos && r.text.find("<Fragment>") < r.text.rfind("</Wix>"),
          "the payloads are inside the document");
}

void a_template_without_the_replaced_line_is_refused() {
    using huxerui::rules::sources::windows_package_definition;
    const auto r = windows_package_definition("<Wix><Package Name=\"@PROJECT_NAME@\" /></Wix>", installer_values());
    check(!r.error.empty(), "a Package.wxs.in that names no icon or harvest is refused");
}

void resource_namespaces_match_what_the_cli_writes_for_cmake() {
    // `huxerui create library` flattens Package::Product the same way.
    check(library_resource_namespace("huxerui", "live2d") == "huxerui_live2d", "namespace_name");
    check(library_resource_namespace("HuxerUI", "Live2D") == "huxerui_live2d", "lower-cased");
    check(library_resource_namespace("", "example-widgets") == "example_widgets", "no namespace keeps the name");
    // A build program is told `mcpplibs` for a package that states no namespace; the application reading that
    // package's manifest sees none. Both must derive the same name.
    check(library_resource_namespace("mcpplibs", "example-widgets") == "example_widgets", "the default namespace is not a segment");
    check(library_resource_namespace("widgets", "widgets") == "widgets", "equal segments are one");
    check(huxerui::rules::sources::application_resource_namespace == "app", "an application's is app");
}

void a_dependency_manifest_says_whether_it_is_a_huxerui_library() {
    const auto live2d = read_dependency_manifest(
        "# a comment\n[package]\nname        = \"live2d\"\nnamespace   = \"huxerui\"\n"
        "version = \"0.1.0\"\n\n[dependencies]\nhuxerui.huxerui = { git = \"x\" }\n");
    check(live2d.name == "live2d" && live2d.package_namespace == "huxerui", "the identity is read");
    check(live2d.uses_huxerui, "a dotted huxerui.huxerui dependency is seen");

    const auto table = read_dependency_manifest(
        "[package]\nname = \"w\"\n[target.'cfg(os = \"ios\")'.dependencies.huxerui]\nhuxerui = \"0.3.0\"\n");
    check(table.uses_huxerui, "a [*.dependencies.huxerui] table is seen");

    const auto tool = read_dependency_manifest(
        "[package]\nname = \"t\"\n[build-dependencies]\nhuxerui.huxerui = { path = \"..\" }\n");
    check(!tool.uses_huxerui, "a build dependency on the framework is not a runtime use");

    const auto plain = read_dependency_manifest("[package]\nname = \"fmt\"\n[dependencies]\nhuxerui-like = \"1\"\n");
    check(!plain.uses_huxerui, "a key that only starts with huxerui is not the framework");
}

void dependency_keys_are_read_in_declaration_order() {
    const auto keys = dependency_keys(
        "[package]\nname = \"app\"\n\n[dependencies]\nhuxerui.huxerui = { path = \"../..\" }\n"
        "example-widgets = { path = \"widgets\" }\n"
        "[target.'cfg(any(os = \"macos\", os = \"ios\"))'.dependencies]\nllvm.libcxx = \"22.1.8.3\"\n"
        "[dependencies.compat]\nfmt = \"11\"\n"
        "[build-dependencies]\ninstaller = { path = \"x\" }\n"
        "[target.'cfg(os = \"windows\")'.feature-deps.windows-installer]\ninstaller = { path = \"x\" }\n"
        "[targets.app]\nkind = \"app\"\n");
    check((keys == std::vector<std::string>{"huxerui.huxerui", "example-widgets", "llvm.libcxx", "compat.fmt"}),
          "runtime dependency keys, qualified, in order, and no build or feature dependency");
}

int main() {
    sha1_matches_the_standard_vectors();
    upgrade_codes_are_the_ones_cmake_derives();
    package_definition_is_cmakes_with_the_staged_files();
    bundle_definition_is_cmakes_with_the_interface_and_payloads();
    a_template_without_the_replaced_line_is_refused();
    header_scan_finds_namespace_scope_declarations();
    header_scan_sees_past_an_attribute();
    header_scan_ignores_the_detail_namespace();
    header_scan_survives_braces_in_literals_and_macros();
    header_scan_handles_templates_and_qualified_names();
    strip_noncode_removes_what_it_should();
    cmake_lists_keep_quoted_tokens_whole();
    macro_definitions_come_out_verbatim();
    umbrella_includes_are_read_in_order();
    module_interface_is_read_from_the_source();
    package_names_become_identifiers();
    glob_single_star_stays_within_one_segment();
    glob_double_star_crosses_separators();
    glob_anchors_at_both_ends();
    codegen_prefilter_matches_the_cmake_rule();
    entry_is_excluded_from_the_transform_set();
    resource_outputs_predict_what_hrc_writes();
    resource_namespaces_match_what_the_cli_writes_for_cmake();
    a_dependency_manifest_says_whether_it_is_a_huxerui_library();
    dependency_keys_are_read_in_declaration_order();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all source-selection checks passed\n";
    return 0;
}
