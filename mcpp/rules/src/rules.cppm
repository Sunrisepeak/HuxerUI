// Build rules for HuxerUI projects driven by mcpp.
//
// A consumer's build.mcpp is expected to be one line:
//
//     import mcpp; import huxerui.rules;
//     int main() { return huxerui::rules::configure({ .resources = "resources" }) ? 0 : 1; }
//
// Everything this module schedules mirrors a step CMake performs in
// cmake/HuxerUICodegen.cmake and cmake/HuxerUIResources.cmake. Where the two
// must agree, the CMake function is named in a comment so a reader can diff
// them; mcpp/parity/check_parity.py mechanically checks the parts that can be
// checked.

export module huxerui.rules;

// C++20, not C++23. A host module is compiled with the ROOT package's
// `standard` -- module-graph-global, dependencies included -- and HuxerUI's is
// c++20 because that is the SDK ABI baseline. So no std::println here.
import std;
import mcpp;

export namespace huxerui::rules {

// ---------------------------------------------------------------- options --
// Field-for-field the arguments of huxerui_add_app() in cmake/HuxerUIApp.cmake,
// so the two spellings of "an application" can be reviewed side by side.
struct options {
    std::vector<std::string> sources;             // default: src/**/*.cpp
    bool                     codegen = true;      // = huxerui_enable_codegen()
    std::string              resources;           // = RESOURCES (root; empty = none)
    std::string              resource_namespace;  // = RESOURCE_NAMESPACE
    std::string              bundle_name;         // = BUNDLE_NAME
    std::string              bundle_identifier;   // = BUNDLE_IDENTIFIER
};

// A planned build-graph edge, handed back so a caller that needs to adjust one
// can. A rule without this pair has a cliff: past its last knob the only way
// out is to hand-write the action, and that copy then drifts.
struct edge {
    std::string              id;
    std::string              role;
    std::string              description;
    std::vector<std::string> command;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
};

// ------------------------------------------------------------ SDK locating --
namespace detail {

inline std::string env_or(const char* name, std::string_view fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string(fallback);
}

// Split a triple into its coordinates. mcpp publishes these directly, so this
// is only a fallback for an engine too old to set them.
inline std::string host_os() {
    std::string h = mcpp::host();
    if (h.find("linux")   != std::string::npos) return "linux";
    if (h.find("windows") != std::string::npos) return "windows";
    if (h.find("macos")   != std::string::npos ||
        h.find("darwin")  != std::string::npos ||
        h.find("apple")   != std::string::npos) return "macos";
    return {};
}

inline std::string host_arch_segment() {
    std::string h = mcpp::host();
    auto dash = h.find('-');
    return dash == std::string::npos ? h : h.substr(0, dash);
}

} // namespace detail

// The SDK root: the directory holding include/, resources/ and tools/prebuilt/.
//
// Two callers, two answers, and neither may guess. When HuxerUI builds itself
// the root is its own manifest directory. When an application builds, the root
// is the resolved install directory of the `huxerui` dependency -- which is a
// SOURCE root, not a build output directory: mcpp deliberately offers no
// channel from a dependency's MCPP_OUT_DIR to a consumer's build.
inline std::string sdk_root() {
    const std::string self = mcpp::manifest_dir();
    if (!self.empty() && std::filesystem::exists(
            std::filesystem::path(self) / "include" / "huxerui" / "huxerui.h")) {
        return self;
    }
    const std::string dep = mcpp::dep_dir("huxerui");
    return dep;
}

// tools/prebuilt/<platform>/<architecture>/<tool>[.exe]
//
// Mirrors _huxerui_resolve_host() + huxerui_resolve_host_tool() in
// cmake/HuxerUICodegen.cmake, including its architecture spelling: linux uses
// `aarch64` and macOS uses `arm64` for the same machine.
//
// Using the committed prebuilt rather than building the tool from source is
// deliberate: it is the SAME binary the CMake build runs, so the two build
// systems cannot produce different generated code.
inline std::string host_tool(std::string_view tool) {
    const std::string root = sdk_root();
    if (root.empty()) return {};

    const std::string os   = detail::host_os();
    const std::string arch = detail::host_arch_segment();

    std::string platform_dir = os;
    std::string arch_dir;
    if (arch == "x86_64" || arch == "amd64" || arch == "x64") {
        arch_dir = "x86_64";
    } else if (arch == "aarch64" || arch == "arm64") {
        arch_dir = (os == "macos") ? "arm64" : "aarch64";
    }
    if (platform_dir.empty() || arch_dir.empty()) return {};

    std::filesystem::path p = std::filesystem::path(root) / "tools" / "prebuilt"
                            / platform_dir / arch_dir;
    p /= std::string(tool) + (os == "windows" ? ".exe" : "");
    return std::filesystem::exists(p) ? p.string() : std::string{};
}

// ------------------------------------------------------------------ globs --
namespace detail {

// Minimal glob over `**`, `*` and literal segments -- enough for the source
// patterns a manifest writes, and no more.
inline bool match_here(std::string_view pat, std::string_view text) {
    std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == text[t])) { ++p; ++t; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; mark = t; }
        else if (star != std::string_view::npos) { p = star + 1; t = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

inline void collect(const std::filesystem::path& root, std::string_view pattern,
                    std::vector<std::string>& out) {
    std::error_code ec;
    const bool recursive = pattern.find("**") != std::string_view::npos;
    auto consider = [&](const std::filesystem::path& f) {
        const std::string rel =
            std::filesystem::relative(f, root, ec).generic_string();
        if (ec) return;
        std::string pat(pattern);
        // `**/` and `*/` are both handled by the flat matcher once `/` is not
        // special; that is sufficient because every pattern here is anchored.
        std::string flat;
        for (std::size_t i = 0; i < pat.size(); ++i) {
            if (pat[i] == '*' && i + 1 < pat.size() && pat[i + 1] == '*') {
                flat += '*'; ++i;
                if (i + 1 < pat.size() && pat[i + 1] == '/') ++i;
                continue;
            }
            flat += pat[i];
        }
        if (match_here(flat, rel)) out.push_back(rel);
    };
    if (recursive) {
        for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file(ec)) consider(it->path());
        }
    } else {
        for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file(ec)) consider(it->path());
        }
    }
    std::ranges::sort(out);
    out.erase(std::ranges::unique(out).begin(), out.end());
}

inline std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

} // namespace detail

// ---------------------------------------------------------------- codegen --
namespace detail {

// The same pre-filter cmake/HuxerUICodegen.cmake applies with file(READ) +
// string(FIND): a source with neither marker gets no transform edge at all.
// Without it every source in the project would grow a needless graph node.
inline bool needs_codegen(const std::string& text) {
    return text.find("[[huxerui::composable]]") != std::string::npos
        || text.find("Use") != std::string::npos;
}

// cmake/HuxerUICodegen.cmake ends by suppressing the diagnostic that
// [[huxerui::composable]] provokes, because no compiler knows the attribute.
inline void suppress_attribute_warning() {
    const std::string os = mcpp::target_os();
    if (os == "windows" && std::string_view(mcpp::compiler()).find("msvc")
                           != std::string_view::npos) {
        mcpp::cxxflag("/wd5030");
    } else if (std::string_view(mcpp::compiler()).find("gcc")
               != std::string_view::npos) {
        mcpp::cxxflag("-Wno-attributes");
    } else {
        mcpp::cxxflag("-Wno-unknown-attributes");
    }
}

} // namespace detail

// Plan one hcg edge per source that actually contains a composable.
inline std::vector<edge> plan_codegen(std::span<const std::string> sources) {
    std::vector<edge> out;
    const std::string hcg = host_tool("hcg");
    if (hcg.empty()) return out;

    const std::filesystem::path root = mcpp::manifest_dir();
    const std::string           odir = std::string(mcpp::out_dir()) + "/hcg";

    for (const std::string& rel : sources) {
        const std::filesystem::path abs = root / rel;
        const std::string text = detail::read_file(abs);
        if (text.empty() || !detail::needs_codegen(text)) continue;

        std::string stem = abs.stem().string();
        std::string flat = rel;
        for (char& c : flat) if (c == '/' || c == '\\') c = '_';
        const std::string gen = odir + "/" + flat;

        out.push_back(edge{
            .id          = "hcg:" + stem,
            .role        = "source",
            .description = "huxerui composable " + rel,
            .command     = { hcg, "--input", abs.string(), "--output", gen },
            .inputs      = { abs.string(), hcg },
            .outputs     = { gen },
        });
    }
    return out;
}

// --------------------------------------------------------------- resources --
// Two hrc invocations plus a merge, which is the shape of
// cmake/HuxerUIResourceBuild.cmake: compile each root, then merge the packages.
//
// The framework's own builtin package is compiled HERE, in the consumer's
// build, rather than reused from the framework's build. It has to be: the
// framework generates it into its own MCPP_OUT_DIR, and a consumer is given
// dep_dir() -- the dependency's SOURCE root. `resources/` is source, so it is
// reachable; the compiled package is not. Recompiling 44 files / 196 KB is the
// cost of not inventing a channel mcpp deliberately does not have.
inline std::vector<edge> plan_resources(const options& opt) {
    std::vector<edge> out;
    const std::string hrc = host_tool("hrc");
    if (hrc.empty()) return out;

    const std::string root = sdk_root();
    const std::string odir = std::string(mcpp::out_dir()) + "/hrc";
    const std::string builtin_src = root + "/resources";

    std::vector<std::string> packages;

    if (std::filesystem::exists(builtin_src)) {
        std::vector<std::string> inputs{ hrc };
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(builtin_src, ec);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file(ec)) inputs.push_back(it->path().string());
        }
        out.push_back(edge{
            .id          = "hrc:builtin",
            .role        = "source",
            .description = "huxerui builtin resources",
            .command     = { hrc, "--root", builtin_src,
                             "--output", odir + "/builtin",
                             "--namespace", "huxerui",
                             "--header-name", "huxerui_builtin_resources.h" },
            .inputs      = inputs,
            .outputs     = { odir + "/builtin/include/huxerui_builtin_resources.h",
                             odir + "/builtin/package/huxerui/resources.bin" },
        });
        packages.push_back(odir + "/builtin/package");
    }

    if (!opt.resources.empty()) {
        const std::string ns =
            opt.resource_namespace.empty() ? std::string(mcpp::package_name())
                                           : opt.resource_namespace;
        // ABSOLUTE. An action's command runs with the BUILD directory as its
        // working directory, not the package root, so a relative root reaches
        // hrc as "resource root is not a directory: resources".
        const std::string app_src =
            (std::filesystem::path(mcpp::manifest_dir()) / opt.resources).string();
        std::vector<std::string> inputs{ hrc };
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(app_src, ec);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file(ec)) inputs.push_back(it->path().string());
        }
        out.push_back(edge{
            .id          = "hrc:app",
            .role        = "source",
            .description = "application resources " + opt.resources,
            .command     = { hrc, "--root", app_src,
                             "--output", odir + "/app",
                             "--namespace", ns },
            .inputs      = inputs,
            .outputs     = { odir + "/app/package/huxerui/resources.bin" },
        });
        packages.push_back(odir + "/app/package");
    }

    if (packages.size() > 1) {
        edge merge{
            .id          = "hrc:merge",
            .role        = "source",
            .description = "merge huxerui resource packages",
            .command     = { hrc, "merge" },
            .inputs      = {},
            .outputs     = { odir + "/final/huxerui/resources.bin" },
        };
        for (const std::string& p : packages) {
            merge.command.push_back("--input");
            merge.command.push_back(p);
            merge.inputs.push_back(p + "/huxerui/resources.bin");
        }
        merge.command.push_back("--output");
        merge.command.push_back(odir + "/final");
        out.push_back(std::move(merge));
    }
    return out;
}

// ----------------------------------------------------------------- submit --
inline bool submit(std::span<const edge> edges) {
    for (const edge& e : edges) {
        mcpp::action a;
        a.id          = e.id.c_str();
        a.role        = e.role.c_str();
        a.description = e.description.c_str();
        for (const std::string& c : e.command) a.arg(c.c_str());
        for (const std::string& i : e.inputs)  a.input(i.c_str());
        for (const std::string& o : e.outputs) a.output(o.c_str());
        a.submit();
    }
    return true;
}

// -------------------------------------------------------------- configure --
// One call for the common case. Returns false only on a condition that should
// stop the build; a missing optional input is reported and tolerated.
inline bool configure(options opt = {}) {
    const std::string root = sdk_root();
    if (root.empty()) {
        std::cerr << "huxerui.rules: cannot locate the HuxerUI SDK root "
                     "(neither this package nor a `huxerui` dependency)\n";
        return false;
    }

    // Re-run when the SET of sources or resources changes; the edges below
    // track content.
    mcpp::rerun_if_changed_glob("src/**/*.cpp");
    if (!opt.resources.empty()) {
        mcpp::rerun_if_changed_glob((opt.resources + "/**").c_str());
    }

    std::vector<edge> edges;

    if (opt.codegen) {
        std::vector<std::string> sources = opt.sources;
        if (sources.empty()) {
            detail::collect(mcpp::manifest_dir(), "src/**/*.cpp", sources);
        }
        auto cg = plan_codegen(sources);
        if (!cg.empty()) {
            detail::suppress_attribute_warning();
            // A transformed source is compiled from MCPP_OUT_DIR, so its own
            // relative #includes no longer resolve. CMake solves this with
            // target_include_directories(PRIVATE <source dir>); the same
            // directories have to be added here or the moved file stops
            // finding its neighbours.
            std::vector<std::string> seen;
            for (const std::string& rel : sources) {
                std::filesystem::path d =
                    (std::filesystem::path(mcpp::manifest_dir()) / rel).parent_path();
                std::string s = d.string();
                if (std::ranges::find(seen, s) == seen.end()) {
                    seen.push_back(s);
                    mcpp::include_dir(s.c_str());
                }
            }
        }
        edges.insert(edges.end(), cg.begin(), cg.end());
    }

    auto rs = plan_resources(opt);
    if (!rs.empty()) {
        mcpp::include_dir(
            (std::string(mcpp::out_dir()) + "/hrc/builtin/include").c_str());
    }
    edges.insert(edges.end(), rs.begin(), rs.end());

    return submit(edges);
}

// The framework's own build needs only the builtin resource header: 11 of its
// translation units #include "huxerui_builtin_resources.h", so this cannot be
// deferred to the consumer even though the compiled package is also produced
// here and thrown away.
inline bool builtin_resources() {
    const std::string hrc = host_tool("hrc");
    if (hrc.empty()) {
        std::cerr << "huxerui.rules: no prebuilt hrc for this host "
                     "(tools/prebuilt/<platform>/<arch>/hrc)\n";
        return false;
    }
    const std::string odir = std::string(mcpp::out_dir()) + "/hrc";
    const std::string src  = std::string(mcpp::manifest_dir()) + "/resources";

    mcpp::rerun_if_changed_glob("resources/**");

    std::vector<std::string> inputs{ hrc };
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(src, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file(ec)) inputs.push_back(it->path().string());
    }

    edge e{
        .id          = "hrc:builtin",
        .role        = "source",
        .description = "huxerui builtin resources",
        .command     = { hrc, "--root", src,
                         "--output", odir + "/builtin",
                         "--namespace", "huxerui",
                         "--header-name", "huxerui_builtin_resources.h" },
        .inputs      = inputs,
        .outputs     = { odir + "/builtin/include/huxerui_builtin_resources.h",
                         odir + "/builtin/package/huxerui/resources.bin" },
    };
    mcpp::include_dir((odir + "/builtin/include").c_str());
    return submit(std::span<const edge>(&e, 1));
}

} // namespace huxerui::rules
