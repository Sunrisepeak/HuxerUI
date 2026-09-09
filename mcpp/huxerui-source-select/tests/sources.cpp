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
using huxerui::rules::sources::without_entry;

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

} // namespace

int main() {
    package_names_become_identifiers();
    glob_single_star_stays_within_one_segment();
    glob_double_star_crosses_separators();
    glob_anchors_at_both_ends();
    codegen_prefilter_matches_the_cmake_rule();
    entry_is_excluded_from_the_transform_set();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all source-selection checks passed\n";
    return 0;
}
