// Which project sources exist, and which of them need the composable transform.
//
// Pure functions of their arguments: no mcpp, no filesystem writes, nothing
// that needs a build program's environment. That is the point -- see the
// package manifest.

export module huxerui.rules.sources;

import std;

export namespace huxerui::rules::sources {

// Glob matching over `**`, `*` and `?`, anchored at both ends.
//
// SEGMENT-WISE, and it has to be. `*` must not cross a directory separator
// while `**` must, and a single-star backtracking matcher cannot express both:
// with `src/**/*.cpp` against `src/a/b/view.cpp` the inner `*` backtracks onto
// a `/`, fails, and has no way to hand the failure back to the outer `**`. The
// unit tests in this package caught exactly that.
//
// Splitting on `/` and letting `**` consume zero or more whole segments makes
// the two spellings mean different things, which is what a manifest that writes
// `src/*/*.cpp` beside `src/**/*.cpp` is relying on.
[[nodiscard]] inline std::vector<std::string_view> split(std::string_view value) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = value.find('/', start);
        if (slash == std::string_view::npos) {
            parts.push_back(value.substr(start));
            return parts;
        }
        parts.push_back(value.substr(start, slash - start));
        start = slash + 1;
    }
}

// One segment: `*` and `?` never see a separator here, so ordinary
// backtracking is enough.
[[nodiscard]] inline bool match_segment(std::string_view pattern, std::string_view text) {
    std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

[[nodiscard]] inline bool match_parts(std::span<const std::string_view> pattern,
                                      std::span<const std::string_view> text) {
    if (pattern.empty()) return text.empty();
    if (pattern.front() == "**") {
        // Zero or more whole segments, so one glob covers a flat project and a
        // nested one.
        for (std::size_t taken = 0; taken <= text.size(); ++taken) {
            if (match_parts(pattern.subspan(1), text.subspan(taken))) return true;
        }
        return false;
    }
    if (text.empty()) return false;
    if (!match_segment(pattern.front(), text.front())) return false;
    return match_parts(pattern.subspan(1), text.subspan(1));
}

[[nodiscard]] inline bool matches(std::string_view pattern, std::string_view text) {
    const std::vector<std::string_view> patternParts = split(pattern);
    const std::vector<std::string_view> textParts = split(text);
    return match_parts(patternParts, textParts);
}

// The same pre-filter cmake/HuxerUICodegen.cmake applies with file(READ) plus
// string(FIND): a source with neither marker gets no transform edge at all.
// Without it every source in a project would grow a needless graph node.
[[nodiscard]] inline bool needs_codegen(std::string_view source) {
    return source.find("[[huxerui::composable]]") != std::string_view::npos
        || source.find("Use") != std::string_view::npos;
}

// `sources` minus the target's entry. A build program can ADD a source but
// cannot replace one, so a transformed copy of the entry would link beside the
// original as `multiple definition of main`.
[[nodiscard]] inline std::vector<std::string>
without_entry(std::span<const std::string> sources, std::string_view entry) {
    std::vector<std::string> out;
    out.reserve(sources.size());
    for (const std::string& source : sources) {
        if (source != entry) out.push_back(source);
    }
    return out;
}

// A package name is not a C++ identifier: `mod-proj` is a perfectly good
// package name and hrc rejects it as a resource namespace
// ("resource namespace must be a non-reserved C++ identifier"). Hyphens and
// dots become underscores, and a leading digit gets a prefix.
[[nodiscard]] inline std::string identifier(std::string_view name) {
    std::string out;
    out.reserve(name.size() + 1);
    for (const char character : name) {
        const bool ok = (character >= 'a' && character <= 'z')
                     || (character >= 'A' && character <= 'Z')
                     || (character >= '0' && character <= '9')
                     || character == '_';
        out.push_back(ok ? character : '_');
    }
    if (out.empty()) return "resources";
    if (out.front() >= '0' && out.front() <= '9') out.insert(out.begin(), '_');
    return out;
}

// The layout `xim:wix` installs, expressed once.
//
// Each of WiX's three NuGet payloads keeps its own directory shape under a
// subdirectory of the package root, so these are the upstream-documented paths
// rather than a repackaging. They are here rather than in the rule so they can
// be tested: a wrong path surfaces on Windows only, at link time, as a missing
// .lib -- and there is no Windows machine on the way to that discovery.
struct wix_layout {
    std::string tool;                   // wix.exe
    std::string bootstrapper_include;   // BootstrapperApplication.h and friends
    std::string bootstrapper_lib;       // balutil.lib
    std::string bootstrapper_runtime;   // mbanative.dll, deployed beside the BA
    std::string dutil_include;          // dutil.h and friends
    std::string dutil_lib;              // dutil.lib
};

[[nodiscard]] inline wix_layout wix_paths(std::string_view root) {
    const std::string r(root);
    return wix_layout{
        .tool                 = r + "/tool/tools/net6.0/any/wix.exe",
        .bootstrapper_include = r + "/bootstrapper/build/native/include",
        .bootstrapper_lib     = r + "/bootstrapper/build/native/v14/x64/balutil.lib",
        .bootstrapper_runtime = r + "/bootstrapper/runtimes/win-x64/native/mbanative.dll",
        .dutil_include        = r + "/dutil/build/native/include",
        .dutil_lib            = r + "/dutil/build/native/v14/x64/dutil.lib",
    };
}

} // namespace huxerui::rules::sources
