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
// them; `huxerui-build-check` mechanically checks the parts that can be
// checked.

export module huxerui.rules;

// C++20, not C++23. A host module is compiled with the ROOT package's
// `standard` -- module-graph-global, dependencies included -- and HuxerUI's is
// c++20 because that is the SDK ABI baseline. So no std::println here.
import std;
import mcpp;
import huxerui.rules.sources;
// Re-exported: a consumer's `import huxerui.rules;` sees the option types,
// provide_formats() and linux_gtk() under huxerui::rules.
export import huxerui.rules.dist;
export import huxerui.rules.gtk;

export namespace huxerui::rules {

// ---------------------------------------------------------------- options --
// Field-for-field the arguments of huxerui_add_app() in cmake/HuxerUIApp.cmake,
// so the two spellings of "an application" can be reviewed side by side. The
// distribution option types are huxerui.rules.dist's.
struct options {
    // The `app` target this build program serves. Names the `<target>.resources`
    // directory the desktop runtime reads beside the executable, and is the
    // target every dist member packages. Empty means the package name, which
    // is the target `mcpp pack` selects by convention.
    std::string              target;
    std::vector<std::string> sources;             // default: src/**/*.cpp
    // The bin target's entry, which mcpp compiles separately from `sources`.
    // It is EXCLUDED from the transform set: a build program can add sources
    // but cannot replace one, so a transformed copy of the entry would link
    // beside the original as `multiple definition of 'main'`. AGENTS.md
    // already says not to annotate the app root, so this costs nothing.
    std::string              entry = "src/main.cpp";
    bool                     codegen = true;      // = huxerui_enable_codegen()
    std::string              resources;           // = RESOURCES (root; empty = none)
    std::string              resource_namespace;  // = RESOURCE_NAMESPACE
    std::string              bundle_name;         // = BUNDLE_NAME
    std::string              bundle_identifier;   // = BUNDLE_IDENTIFIER
    // The distribution formats. Each is provided on the row it serves with
    // nothing stated -- `mcpp pack --format msi|appimage|app|web|apk` works
    // on a fresh project -- and these only change what it produces.
    installer_options        installer;           // Windows MSI and Setup.exe
    appimage_options         appimage;            // Linux AppImage
    apple_options            apple;               // macOS / iOS .app
    web_options              web;                 // the Web page
    android_options          android;             // the APK
    // THE WINDOWS INSTALLER INTERFACE rather than an application: the program
    // an application's Setup.exe runs, as huxerui_add_windows_installer() in
    // cmake/HuxerUIWindowsInstaller.cmake builds `<target>-Installer.exe` --
    // this package's composables and `installer` resources beside the SDK's
    // Burn glue and the WiX bootstrapper SDK. A package without `src/app.cpp`
    // or without `resources/` is given the ones the SDK's Windows template
    // gives a CMake project, with `bundle_name` as the product. Windows only.
    bool                     bootstrapper = false;
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
    // A generated MODULE INTERFACE has to declare what it will provide and
    // import: mcpp fixes the module graph during prepare, before the generator
    // runs, and seeds a placeholder carrying exactly this declaration. Empty
    // for an ordinary generated source.
    std::string              provides;
    std::vector<std::string> imports;
    // Where the command reports the files it read. An action's `inputs` are
    // fixed before the command runs and travel through a fixed-size buffer;
    // a depfile is neither, so a command that reads a whole directory tree
    // says so here instead of enumerating it. NEVER also an output: ninja
    // consumes and deletes the file, so an edge that promised it would be
    // permanently dirty.
    std::string              depfile;
};

// ------------------------------------------------------------ SDK locating --
namespace detail {

inline std::string env_or(const char* name, std::string_view fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string(fallback);
}

} // namespace detail

// The SDK root: the directory holding include/ and resources/.
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

// The host tools, built from source by mcpp and handed here as absolute paths.
//
// NOT tools/prebuilt/. Those binaries are produced for the host's C library by
// .github/workflows/update-host-tools.yml, while mcpp compiles with its own
// payload toolchain and its own glibc; and after a change to
// tools/codegen/transform.cpp they are stale until that workflow runs on main.
// Building from source keeps the mcpp leg self-consistent and takes nothing
// from the machine. CMake keeps using tools/prebuilt/ exactly as before.
//
// `tools = ["hcg"]` on the dependency edge is what asks for this; `reexport`
// on the same edge is what makes it reach an APPLICATION's build program
// without the application declaring the tool packages itself.
inline std::string host_tool(std::string_view tool) {
    if (tool == "hcg")
        return mcpp::dep_bin("huxerui-codegen", "hcg");
    if (tool == "hrc")
        return mcpp::dep_bin("huxerui-resource-compiler", "hrc");
    return {};
}

// -------------------------------------------------------- instantiations --
namespace detail {

// GCC 16 emits a template instantiation an imported inline function needs
// only in a translation unit that instantiated the class for itself. A build
// program with no std::vector<std::string> of its own -- the common one-line
// `configure({})` -- then fails to link the rule it called:
//
//     undefined reference to `std::vector<std::string>::vector(
//         std::initializer_list<std::string>, const std::allocator&)'
//
// A function defined in a module interface unit is not implicitly inline, so
// this one is emitted in the module's own object, and with it the
// instantiation every edge's `.command = { ... }` needs.
std::vector<std::string> pinned_strings() { return { std::string("pin"), "instantiation" }; }

} // namespace detail

// ------------------------------------------------------------------ globs --
namespace detail {

inline void collect(const std::filesystem::path& root, std::string_view pattern,
                    std::vector<std::string>& out) {
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = std::filesystem::relative(it->path(), root, ec).generic_string();
        if (ec) continue;
        if (huxerui::rules::sources::matches(pattern, rel)) out.push_back(rel);
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

// Written only when the content differs, so an unchanged file keeps its
// timestamp and nothing downstream of it rebuilds.
inline bool write_if_different(const std::filesystem::path& p, std::string_view content) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(p, ec) && read_file(p) == content) return true;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(out);
}

// A copy of a template file, or of a tree of them, with `@PROJECT_NAME@`
// substituted -- the one token the SDK's Windows installer template carries in
// its sources and strings -- which is what a CMake project holds once the CLI
// has rendered that template into it.
inline bool render_file(const std::filesystem::path& from, const std::filesystem::path& to,
                        std::string_view project_name) {
    std::string text = read_file(from);
    if (text.empty()) return false;
    for (std::size_t at = 0; (at = text.find("@PROJECT_NAME@", at)) != std::string::npos;
         at += project_name.size())
        text.replace(at, 14, project_name);
    return write_if_different(to, text);
}

inline bool render_tree(const std::filesystem::path& from, const std::filesystem::path& to,
                        std::string_view project_name) {
    std::error_code ec;
    bool any = false;
    for (auto it = std::filesystem::recursive_directory_iterator(from, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) return false;
        if (!it->is_regular_file(ec)) continue;
        if (!render_file(it->path(), to / std::filesystem::relative(it->path(), from, ec), project_name))
            return false;
        any = true;
    }
    return any && !ec;
}

} // namespace detail

// ---------------------------------------------------------------- codegen --
namespace detail {

using huxerui::rules::sources::needs_codegen;

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

// Plan one hcg edge per source that actually contains a composable. `sources`
// are relative to `root`, which is the package's own directory except for the
// sources the SDK gives a Windows installer interface (configure()).
inline std::vector<edge> plan_codegen(std::span<const std::string> sources,
                                      const std::filesystem::path& root = mcpp::manifest_dir()) {
    std::vector<edge> out;
    const std::string hcg = host_tool("hcg");
    if (hcg.empty()) return out;

    const std::string           odir = std::string(mcpp::out_dir()) + "/hcg";

    for (const std::string& rel : sources) {
        const std::filesystem::path abs = root / rel;
        const std::string text = detail::read_file(abs);
        if (text.empty() || !detail::needs_codegen(text)) continue;

        std::string stem = abs.stem().string();
        std::string flat = rel;
        for (char& c : flat) if (c == '/' || c == '\\') c = '_';
        const std::string gen = odir + "/" + flat;

        edge e{
            .id          = "hcg:" + stem,
            .role        = "source",
            .description = "huxerui composable " + rel,
            .command     = { hcg, "--input", abs.string(), "--output", gen },
            .inputs      = { abs.string(), hcg },
            .outputs     = { gen },
        };
        if (abs.extension() == ".cppm") {
            const auto declared = huxerui::rules::sources::scan_module_interface(text);
            e.provides = declared.name;
            e.imports  = declared.imports;
        }
        out.push_back(std::move(e));
    }
    return out;
}

// --------------------------------------------------------------- resources --
namespace detail {

// THE ACCESSORS AS A MODULE, `<namespace>.resources`, for a package written
// in module units. hrc writes the header and this unit from one list, so
// such an application writes `import app.resources;` and no header at all
// -- the same mechanism hcg's transformed units use: an action output
// declared as a module interface (`provides`) whose graph node mcpp seeds
// before the generator runs. Header-style packages get the header only:
// the rule forces `<typeinfo>` and the scope prelude into every one of
// their translation units, and a forced include lands before `export
// module`, which no compiler accepts.
inline void add_resources_module(edge& e, const std::string& odir, const std::string& ns) {
    e.command.push_back("--module-name");
    e.command.push_back(ns + ".resources");
    e.outputs.insert(e.outputs.begin(), odir + "/app/modules/" + ns + "_resources.cppm");
    e.provides = ns + ".resources";
    e.imports  = { "huxerui" };
}

// A HuxerUI LIBRARY'S RESOURCES, AS AN APPLICATION MERGES THEM. CMake's
// huxerui_use_library() appends a library's compiled package to the
// application's resource inputs; under mcpp the application's own build program
// finds the same libraries among the dependencies its manifest declares
// (huxerui::rules::direct_libraries()) and compiles each one's `resources/`
// under the namespace huxerui_add_library() would be given
// (sources::library_resource_namespace). A library whose own build program
// names a different root or namespace is not visible here: a build program
// cannot read another package's options (mcpp-community/mcpp#647, E1).
struct library_resources {
    std::string package;
    std::string ns;
    std::string root;
};

inline std::vector<library_resources> direct_library_resources() {
    std::vector<library_resources> out;
    for (const direct_library& library : direct_libraries()) {
        std::error_code ec;
        const std::filesystem::path resources = library.root / "resources";
        if (!std::filesystem::is_directory(resources, ec)) continue;
        // A resource added to or removed from the library changes the payloads
        // this program declares; its content is the hrc edge's depfile's.
        // Directories rather than a glob: a glob matches under this package's
        // own root only.
        mcpp::rerun_if_changed(resources.string().c_str());
        for (auto it = std::filesystem::recursive_directory_iterator(resources, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_directory(ec)) mcpp::rerun_if_changed(it->path().string().c_str());
        }
        out.push_back({ library.key,
                        huxerui::rules::sources::library_resource_namespace(library.manifest.package_namespace,
                                                                            library.manifest.name),
                        resources.string() });
    }
    return out;
}

} // namespace detail

// Two hrc invocations plus a merge, which is the shape of
// cmake/HuxerUIResourceBuild.cmake: compile each root, then merge the packages.
//
// The framework's own builtin package is compiled HERE, in the consumer's
// build, rather than reused from the framework's build. It has to be: the
// framework generates it into its own MCPP_OUT_DIR, and a consumer is given
// dep_dir() -- the dependency's SOURCE root. `resources/` is source, so it is
// reachable; the compiled package is not. Recompiling 44 files / 196 KB is the
// cost of not inventing a channel mcpp deliberately does not have.
inline std::vector<edge> plan_resources(const options& opt, bool application, bool module_units) {
    std::vector<edge> out;
    const std::string hrc = host_tool("hrc");
    if (hrc.empty()) return out;

    // A LIBRARY DEPLOYS NOTHING. Its build program runs the same rule an
    // application's does, but the package the runtime reads is the
    // application's: a library that deployed the builtin package too would
    // place a second copy at the same path -- `runtime deploy collision`,
    // measured on the macOS row where every package's prefix is `HuxerUI/`.
    // A library with resources of its own gets them compiled for its own
    // header here, and an application that depends on it merges the same
    // resources into its package (detail::direct_library_resources()).
    if (!application) {
        if (opt.resources.empty()) return out;
        const std::string ns = opt.resource_namespace.empty()
            ? huxerui::rules::sources::library_resource_namespace(mcpp::package_namespace(), mcpp::package_name())
            : opt.resource_namespace;
        const std::string odir = std::string(mcpp::out_dir()) + "/hrc";
        const std::string src  = (std::filesystem::path(mcpp::manifest_dir()) / opt.resources).string();
        const std::string dep  = odir + "/app.d";
        const std::string bin  = odir + "/app/package/huxerui/resources.bin";
        std::vector<std::string> outputs;
        for (const std::string& p : huxerui::rules::sources::resource_outputs(src, ns))
            outputs.push_back(odir + "/app/package/" + p);
        edge e{
            .id          = "hrc:app",
            .role        = "source",
            .description = "library resources " + opt.resources,
            .command     = { hrc, "--root", src, "--output", odir + "/app", "--namespace", ns,
                             "--depfile", dep, "--depfile-target", bin },
            .inputs      = { hrc },
            .outputs     = outputs,
            .depfile     = dep,
        };
        if (module_units) detail::add_resources_module(e, odir, ns);
        out.push_back(std::move(e));
        // An installer interface's resources are compiled here for its header
        // alone: the package it reads is assembled by the application that
        // ships the interface (plan_setup_interface()).
        return out;
    }

    const std::string root = sdk_root();
    const std::string odir = std::string(mcpp::out_dir()) + "/hrc";
    const std::string builtin_src = root + "/resources";

    std::vector<std::string> packages;

    if (std::filesystem::exists(builtin_src)) {
        // THE RESOURCE FILES ARE NOT ENUMERATED HERE, AND THAT IS THE FIX FOR
        // HuxerUI#130. `mcpp::action` carries its inputs through a fixed 8192
        // byte buffer, and these 44 paths each carry the dependency's unpack
        // prefix -- measured at 8131 bytes from a checkout 149 characters
        // deep, which overflowed by about 45 bytes and made whether a consumer
        // could build depend on how deep their project sat on disk. hrc
        // reports what it read through a depfile instead: ninja folds that in
        // after the first run, so per-file incrementality survives with one
        // declared input rather than 44.
        const std::string dep = odir + "/builtin.d";
        const std::string header = odir + "/builtin/include/huxerui_builtin_resources.h";
        // EVERY PAYLOAD IS A DECLARED OUTPUT (mcpp 2026.9.13.1 lifted the
        // 8192-byte ceiling on the list): each one is deployed beside the
        // executable below, and a copy edge has to name a declared output to
        // be ordered after the command that writes it.
        std::vector<std::string> outputs{ header };
        for (const std::string& p : huxerui::rules::sources::resource_outputs(builtin_src, "huxerui"))
            outputs.push_back(odir + "/builtin/package/" + p);
        out.push_back(edge{
            .id          = "hrc:builtin",
            .role        = "source",
            .description = "huxerui builtin resources",
            .command     = { hrc, "--root", builtin_src,
                             "--output", odir + "/builtin",
                             "--namespace", "huxerui",
                             "--header-name", "huxerui_builtin_resources.h",
                             "--depfile", dep, "--depfile-target", header },
            .inputs      = { hrc },
            .outputs     = outputs,
            .depfile     = dep,
        });
        packages.push_back(odir + "/builtin/package");
    }

    // THE LIBRARIES' RESOURCES, after the framework's and before the
    // application's, in the order the manifest declares the libraries -- the
    // order huxerui_use_library() appends their packages in, so a later package
    // overrides an earlier one's variant of the same resource.
    const std::vector<detail::library_resources> libraries = detail::direct_library_resources();
    for (std::size_t i = 0; i < libraries.size(); ++i) {
        const detail::library_resources& library = libraries[i];
        const std::string lib_out = odir + "/library-" + std::to_string(i);
        std::vector<std::string> outputs;
        for (const std::string& p : huxerui::rules::sources::resource_outputs(library.root, library.ns))
            outputs.push_back(lib_out + "/package/" + p);
        const std::string index = lib_out + "/package/huxerui/resources.bin";
        if (std::ranges::find(outputs, index) == outputs.end()) outputs.push_back(index);
        out.push_back(edge{
            .id          = "hrc:library-" + std::to_string(i),
            .role        = "source",
            .description = "library resources " + library.package,
            .command     = { hrc, "--root", library.root, "--output", lib_out, "--namespace", library.ns,
                             "--depfile", lib_out + ".d", "--depfile-target", index },
            .inputs      = { hrc },
            .outputs     = outputs,
            .depfile     = lib_out + ".d",
        });
        packages.push_back(lib_out + "/package");
    }

    if (!opt.resources.empty()) {
        // `app`, what CMake's application template states as RESOURCE_NAMESPACE;
        // a stated namespace wins.
        const std::string ns =
            opt.resource_namespace.empty()
                ? std::string(huxerui::rules::sources::application_resource_namespace)
                : opt.resource_namespace;
        // ABSOLUTE. An action's command runs with the BUILD directory as its
        // working directory, not the package root, so a relative root reaches
        // hrc as "resource root is not a directory: resources".
        const std::string app_src =
            (std::filesystem::path(mcpp::manifest_dir()) / opt.resources).string();
        // Same shape as hrc:builtin above, and for the same reason: an
        // application's own resource tree is unbounded, so enumerating it puts
        // the 8192 byte ceiling between the project and its own build.
        const std::string dep = odir + "/app.d";
        const std::string bin = odir + "/app/package/huxerui/resources.bin";
        std::vector<std::string> outputs;
        for (const std::string& p : huxerui::rules::sources::resource_outputs(app_src, ns))
            outputs.push_back(odir + "/app/package/" + p);
        edge e{
            .id          = "hrc:app",
            .role        = "source",
            .description = "application resources " + opt.resources,
            .command     = { hrc, "--root", app_src,
                             "--output", odir + "/app",
                             "--namespace", ns,
                             "--depfile", dep, "--depfile-target", bin },
            .inputs      = { hrc },
            .outputs     = outputs,
            .depfile     = dep,
        };
        if (module_units) detail::add_resources_module(e, odir, ns);
        out.push_back(std::move(e));
        packages.push_back(odir + "/app/package");
    }

    // The package the runtime reads: the merge of both when the application
    // has resources, the builtin package alone otherwise. `hrc merge` writes
    // under `<output>/package/`, exactly as `hrc` does -- the previous edge
    // declared `final/huxerui/resources.bin`, a path hrc never wrote, so the
    // merge stayed dirty on every build.
    std::string package_dir = packages.front();
    std::vector<std::string> package_paths;
    if (packages.size() > 1) {
        package_dir = odir + "/final/package";
        std::set<std::string> merged;
        for (const std::string& p : huxerui::rules::sources::resource_outputs(builtin_src, "huxerui"))
            merged.insert(p);
        for (const detail::library_resources& library : libraries)
            for (const std::string& p : huxerui::rules::sources::resource_outputs(library.root, library.ns))
                merged.insert(p);
        if (!opt.resources.empty()) {
            const std::string ns = opt.resource_namespace.empty()
                ? std::string(huxerui::rules::sources::application_resource_namespace) : opt.resource_namespace;
            const std::string app_src =
                (std::filesystem::path(mcpp::manifest_dir()) / opt.resources).string();
            for (const std::string& p : huxerui::rules::sources::resource_outputs(app_src, ns))
                merged.insert(p);
        }
        package_paths.assign(merged.begin(), merged.end());
        edge merge{
            .id          = "hrc:merge",
            .role        = "source",
            .description = "merge huxerui resource packages",
            .command     = { hrc, "merge" },
            .inputs      = {},
            .outputs     = {},
        };
        for (const std::string& p : package_paths) merge.outputs.push_back(package_dir + "/" + p);
        for (const std::string& p : packages) {
            merge.command.push_back("--input");
            merge.command.push_back(p);
            merge.inputs.push_back(p + "/huxerui/resources.bin");
        }
        merge.command.push_back("--output");
        merge.command.push_back(odir + "/final");
        out.push_back(std::move(merge));
    } else {
        package_paths = huxerui::rules::sources::resource_outputs(builtin_src, "huxerui");
    }

    // WHERE THE RUNTIME READS THE PACKAGE, on each row, relative to the
    // executable: `<name>.resources/` on Linux and Windows
    // (linux_adapter.cpp, win32_adapter.cpp), `HuxerUI/` beside the
    // executable on macOS and iOS (appkit_adapter.mm, uikit_adapter.mm), the
    // APK's `assets/` on Android -- dist-apk maps the deployed tree there --
    // and Emscripten's MEMFS root on the Web, which is a link-time preload
    // rather than a copy. `mcpp::deploy` is one file at a time and its `to`
    // is a directory, so each package path is placed under its own dirname.
    const std::string os  = mcpp::target_os();
    const std::string env = mcpp::target_env();
    if (os == "emscripten") {
        mcpp::link_flag("--preload-file");
        mcpp::link_flag((package_dir + "@/").c_str());
    } else {
        const std::string name = opt.target.empty() ? std::string(mcpp::package_name()) : opt.target;
        const std::string root = env == "android" ? std::string(".")
                               : (os == "macos" || os == "ios") ? std::string("HuxerUI")
                               : name + ".resources";
        for (const std::string& p : package_paths) {
            const std::string dir = std::filesystem::path(p).parent_path().generic_string();
            const std::string to = root == "." ? dir : (dir.empty() ? root : root + "/" + dir);
            mcpp::deploy((package_dir + "/" + p).c_str(), to.c_str());
        }
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
        if (!e.provides.empty()) a.provides(e.provides.c_str());
        for (const std::string& i : e.imports) a.imports(i.c_str());
        if (!e.depfile.empty()) a.depfile = e.depfile.c_str();
        a.submit();
    }
    return true;
}

// ------------------------------------------------ Windows installer interface --
namespace detail {

// The SDK's Windows application template, which CMake renders into a project:
// the WiX definitions and the installer interface's page and strings.
inline std::filesystem::path windows_template(const std::string& root) {
    return std::filesystem::path(root) / "tools/huxerui_cli/templates/platform/windows/app";
}

inline std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : text) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

// What an installer interface package does not carry itself, from the SDK's
// Windows template: the page (`src/app.cpp`) and the strings (`resources/`),
// rendered with the product's name into this build's output directory. A
// package that carries its own of either keeps it, as a CMake project owns the
// copy the CLI rendered into it. Sets `default_root` when the page is the
// SDK's.
inline bool interface_defaults(options& opt, const std::string& root, std::filesystem::path& default_root) {
    if (std::string_view(mcpp::target_os()) != "windows") {
        std::cerr << "huxerui.rules: " << mcpp::package_name()
                  << " is a Windows installer interface, which builds for a Windows target only\n";
        return false;
    }
    const std::filesystem::path manifest = mcpp::manifest_dir();
    const std::filesystem::path shell = windows_template(root) / "package";
    const std::string product = !opt.bundle_name.empty() ? opt.bundle_name : std::string(mcpp::package_name());
    const std::filesystem::path out = std::filesystem::path(mcpp::out_dir()) / "installer";
    if (!std::filesystem::is_regular_file(manifest / "src/app.cpp")) {
        default_root = out;
        if (!render_file(shell / "src/app.cpp", out / "src/app.cpp", product)) {
            std::cerr << "huxerui.rules: cannot render " << (shell / "src/app.cpp").string() << "\n";
            return false;
        }
        mcpp::rerun_if_changed((shell / "src/app.cpp").string().c_str());
    }
    if (opt.resources.empty()) {
        if (std::filesystem::is_directory(manifest / "resources")) {
            opt.resources = "resources";
        } else {
            if (!render_tree(shell / "resources", out / "resources", product)) {
                std::cerr << "huxerui.rules: cannot render " << (shell / "resources").string() << "\n";
                return false;
            }
            // Absolute: plan_resources() joins it to the manifest directory,
            // which an absolute path survives.
            opt.resources = (out / "resources").string();
        }
    }
    if (opt.resource_namespace.empty()) opt.resource_namespace = "installer";
    return true;
}

// See options::bootstrapper: the SDK's Burn glue and the WiX bootstrapper SDK,
// as huxerui_add_windows_installer() compiles and links them.
inline bool windows_bootstrapper(const options& opt, const std::string& root) {
    const std::string wix = mcpp::xpkg_dir("xim", "wix");
    if (wix.empty()) {
        std::cerr << "huxerui.rules: the xim:wix payload is not visible to this build program; "
                     "the Windows installer interface compiles against its bootstrapper SDK\n";
        return false;
    }
    const std::string target = opt.target.empty() ? std::string(mcpp::package_name()) : opt.target;
    const std::filesystem::path glue = std::filesystem::path(root) / "platform/windows/windows_installer.cpp";
    const std::string text = read_file(glue);
    if (text.empty()) {
        std::cerr << "huxerui.rules: cannot read " << glue.string() << "\n";
        return false;
    }
    // THE GLUE IS COMPILED THROUGH A ONE-LINE UNIT IN THIS BUILD'S OUTPUT
    // DIRECTORY, NOT NAMED DIRECTLY. mcpp files a source that lies under a
    // dependency's root -- the SDK's -- beneath an object directory mirroring
    // the path from this package to it, `obj/<package>/__up/.../platform/windows/`,
    // and this program is built in mcpp's tool store, whose own prefix is 180
    // characters on windows-2022: the scan wrote the unit's `.ddi` and the next
    // step could not read it back past Windows' 260. A unit under no package
    // root is filed as `obj/<name>`. The digest makes an edit of the glue an
    // edit of the unit, since this row writes no dependency file.
    const std::filesystem::path unit = std::filesystem::path(mcpp::out_dir()) / "installer/huxerui_windows_installer.cpp";
    const std::string content = "// Generated by huxerui.rules: the SDK's Burn glue as this program's own unit.\n"
                                "// " + std::to_string(fnv1a(text)) + "\n"
                                "#include \"" + glue.generic_string() + "\"\n";
    if (!write_if_different(unit, content)) {
        std::cerr << "huxerui.rules: cannot write " << unit.string() << "\n";
        return false;
    }
    mcpp::source(unit.string().c_str());
    mcpp::rerun_if_changed(glue.string().c_str());
    mcpp::include_dir((root + "/platform/windows").c_str());
    mcpp::include_dir((wix + "/bootstrapper/build/native/include").c_str());
    mcpp::include_dir((wix + "/dutil/build/native/include").c_str());
    for (const char* define : { "UNICODE", "_UNICODE", "NOMINMAX", "WIN32_LEAN_AND_MEAN" }) mcpp::define(define);
    mcpp::link_search((wix + "/bootstrapper/build/native/v14/x64").c_str());
    mcpp::link_search((wix + "/dutil/build/native/v14/x64").c_str());
    // balutil and dutil, and the Windows import libraries the xim:wix recipe
    // states their MSVC-ABI link needs; cl reaches the last ones through the
    // archives' default-library records, which not every linker reads.
    for (const char* library : { "balutil", "dutil", "version", "shlwapi", "wininet", "msi", "rpcrt4",
                                 "comctl32", "wintrust" })
        mcpp::link_lib(library);
    // `wWinMain`, as huxerui_add_windows_installer() sets wWinMainCRTStartup.
    // Nothing is deployed beside the program: a tool publishes its binary alone,
    // and Setup.exe carries the files the interface runs beside
    // (plan_setup_interface()).
    mcpp::windows_entry(target.c_str(), "wWinMain");
    return true;
}

// THE INTERFACE A HUXERUI SETUP.EXE RUNS, under `--format setup`: the program
// the application's `installer` package builds, and the files CMake stages
// beside it -- WiX's `mbanative.dll` and the interface's resource package, the
// builtin package merged with its strings.
//
// The program is a host tool of the application's Windows rows, requested only
// by a build that names the `windows-installer` feature:
//
//     [features]
//     windows-installer = []
//
//     [target.'cfg(os = "windows")'.feature-deps.windows-installer]
//     installer = { path = "windows/installer", tools = ["<target>-Installer"] }
//
// As CMake builds it only for a package build (HUXERUI_PACKAGE): an ordinary
// build does not compile it, and `huxerui package windows` runs
// `mcpp pack --format setup --features windows-installer` (mcpp 2026.9.15.2
// passes a pack's features to every build it performs, mcpp#641 item 4). A
// tool publishes its binary and nothing beside it, which is why the resource
// package is compiled here, in this build, and carried as payloads.
inline bool plan_setup_interface(const options& opt, const installer_options& installer,
                                 const std::string& root, installer_interface& out) {
    const std::string target = opt.target.empty() ? std::string(mcpp::package_name()) : opt.target;
    const std::string tool = target + "-Installer";
    out.program = mcpp::dep_bin("installer", tool.c_str());
    if (out.program.empty()) {
        return refuse("huxerui.rules: a HuxerUI Setup.exe runs the application's own installer interface, "
                      "and no `installer` package provided the tool " + tool + " to this build. Pack with "
                      "`mcpp pack --format setup --features windows-installer` (what `huxerui package windows` "
                      "runs), from a manifest that declares it:\n"
                      "  [features]\n"
                      "  windows-installer = []\n"
                      "  [target.'cfg(os = \"windows\")'.feature-deps.windows-installer]\n"
                      "  installer = { path = \"windows/installer\", tools = [\"" + tool + "\"] }");
    }
    const std::filesystem::path native =
        std::filesystem::path(mcpp::xpkg_dir("xim", "wix")) / "bootstrapper/runtimes/win-x64/native/mbanative.dll";
    if (!std::filesystem::is_regular_file(native)) {
        return refuse("huxerui.rules: the xim:wix payload carries no " + native.string() +
                      ", which the installer interface loads");
    }
    out.payloads.emplace_back(native.string(), "mbanative.dll");

    const std::string hrc = host_tool("hrc");
    if (hrc.empty()) return refuse("huxerui.rules: hrc was not provided for the installer interface's resources");
    // The interface package's strings, or the SDK template's when it has none.
    const std::string product = !installer.display_name.empty() ? installer.display_name
                                                                : std::string(mcpp::package_name());
    std::filesystem::path strings = std::filesystem::path(mcpp::dep_dir("installer")) / "resources";
    if (!std::filesystem::is_directory(strings)) {
        strings = std::filesystem::path(mcpp::out_dir()) / "installer-setup/resources";
        if (!render_tree(windows_template(root) / "package/resources", strings, product))
            return refuse("huxerui.rules: cannot render the SDK's installer strings into " + strings.string());
    }
    const std::string odir = std::string(mcpp::out_dir()) + "/hrc";
    const std::string builtin_src = root + "/resources";
    const std::string compiled = odir + "/installer/package";
    const std::string merged = odir + "/installer-final/package";
    std::vector<edge> edges;
    {
        std::vector<std::string> outputs;
        for (const std::string& p : huxerui::rules::sources::resource_outputs(strings, "installer"))
            outputs.push_back(compiled + "/" + p);
        edges.push_back(edge{
            .id          = "hrc:installer",
            .role        = "source",
            .description = "installer interface resources",
            .command     = { hrc, "--root", strings.string(), "--output", odir + "/installer",
                             "--namespace", "installer",
                             "--depfile", odir + "/installer.d", "--depfile-target", compiled + "/huxerui/resources.bin" },
            .inputs      = { hrc },
            .outputs     = outputs,
            .depfile     = odir + "/installer.d",
        });
    }
    std::set<std::string> paths;
    for (const std::string& p : huxerui::rules::sources::resource_outputs(builtin_src, "huxerui")) paths.insert(p);
    for (const std::string& p : huxerui::rules::sources::resource_outputs(strings, "installer")) paths.insert(p);
    edge merge{
        .id          = "hrc:installer-merge",
        .role        = "source",
        .description = "merge the installer interface's resource package",
        .command     = { hrc, "merge", "--input", odir + "/builtin/package", "--input", compiled,
                         "--output", odir + "/installer-final" },
        .inputs      = { odir + "/builtin/package/huxerui/resources.bin", compiled + "/huxerui/resources.bin" },
        .outputs     = {},
    };
    for (const std::string& p : paths) {
        merge.outputs.push_back(merged + "/" + p);
        out.payloads.emplace_back(merged + "/" + p, tool + ".resources/" + p);
    }
    edges.push_back(std::move(merge));
    return submit(edges);
}

} // namespace detail

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

    // A MODULE INTERFACE UNIT CANNOT RECEIVE A FORCED INCLUDE, so this is
    // conditional rather than unconditional.
    //
    // `-include` prepends the header before the first line of the translation
    // unit, and a module interface must open with `module;` or `export module`.
    // Measured on gcc 16.1.0, even a unit that opens with a global module
    // fragment:
    //
    //     error: module-declaration only permitted as first declaration,
    //            or ending a global module fragment
    //
    // A package with module interface units therefore writes its own includes
    // in the global module fragment, which is where a module unit's includes
    // belong anyway. A package without them keeps the clean form, which is the
    // whole point of forcing the include: `import huxerui;` has to be the only
    // difference from `#include <huxerui/huxerui.h>`.
    // An installer interface's page and strings, when the package carries none
    // of its own, before anything below reads `sources` or `resources`.
    std::filesystem::path interface_root;
    if (opt.bootstrapper && !detail::interface_defaults(opt, root, interface_root)) return false;
    const std::vector<std::string> interface_sources =
        interface_root.empty() ? std::vector<std::string>{} : std::vector<std::string>{ "src/app.cpp" };

    std::vector<std::string> selected = opt.sources;
    if (selected.empty()) {
        detail::collect(mcpp::manifest_dir(), "src/**/*.cpp", selected);
        detail::collect(mcpp::manifest_dir(), "src/**/*.cppm", selected);
    }
    selected = huxerui::rules::sources::without_entry(selected, opt.entry);
    const bool has_module_interface = std::ranges::any_of(
        selected, [](const std::string& source) { return source.ends_with(".cppm"); });

    // `import huxerui;` needs <typeinfo> in the IMPORTING translation unit.
    //
    // GCC's `typeid` check is per-TU: it asks whether <typeinfo> was included
    // here, not whether std::type_info is reachable. Entities in the module's
    // global module fragment do not satisfy it, and HuxerUI's UseState(),
    // View and Layout machinery all instantiate typeid in the caller -- so
    // every importer would otherwise open with a line of compiler trivia:
    //
    //     include/huxerui/state.h:463: error: must '#include <typeinfo>'
    //                                         before using 'typeid'
    //
    // Emitting it as a forced include keeps that out of application code,
    // which is the point: `import huxerui;` has to be the ONLY difference from
    // `#include <huxerui/huxerui.h>`. Measured on gcc 16.1.0.
    // THE SCOPE MACROS COME THE SAME WAY, AND FOR A SHARPER REASON.
    //
    // HUXERUI_SCOPE / _BEGIN / _END are public API -- hand-written composables
    // use them directly -- and a macro does not cross a module boundary. So an
    // importing translation unit that writes HUXERUI_SCOPE(...) by hand fails
    // with `'HUXERUI_SCOPE' was not declared in this scope`, no matter that hcg
    // no longer emits the names itself.
    //
    // hcg emitting the expansion fixes GENERATED code; this fixes HAND-WRITTEN
    // code. They are different holes and both have to be closed for
    // `import huxerui;` to behave exactly like `#include <huxerui/huxerui.h>`.
    //
    // The header is generated from view.h by `huxerui-module-gen` and checked
    // by that same program's --check mode, so the two definitions cannot
    // drift into an illegal redefinition for a unit that does both.
    const std::string prelude =
        root + "/mcpp/huxerui-build-rules/include/huxerui_scope_prelude.h";
    const bool msvc =
        std::string_view(mcpp::compiler()).find("msvc") != std::string_view::npos;
    // The prelude's DIRECTORY is on the include path either way, so a module
    // unit can put `#include <huxerui_scope_prelude.h>` in its global module
    // fragment and hand-written HUXERUI_SCOPE works there too. Only the
    // FORCING is conditional.
    mcpp::include_dir(
        (root + "/mcpp/huxerui-build-rules/include").c_str());

    if (has_module_interface) {
        // Nothing forced; the module units carry it themselves.
    } else if (msvc) {
        mcpp::cxxflag("/FItypeinfo");
        mcpp::cxxflag(("/FI" + prelude).c_str());
    } else {
        mcpp::cxxflag("-include");
        mcpp::cxxflag("typeinfo");
        mcpp::cxxflag("-include");
        mcpp::cxxflag(prelude.c_str());
    }
    mcpp::rerun_if_changed(prelude.c_str());

    // Re-run when the SET of sources or resources changes; the edges below
    // track content.
    mcpp::rerun_if_changed_glob("src/**/*.cpp");
    mcpp::rerun_if_changed_glob("src/**/*.cppm");
    if (!opt.resources.empty() && !std::filesystem::path(opt.resources).is_absolute()) {
        mcpp::rerun_if_changed_glob((opt.resources + "/**").c_str());
    }

    std::vector<edge> edges;

    if (opt.codegen) {
        const std::vector<std::string>& sources = selected;

        // THE MANIFEST MUST SAY `sources = []`, AND THIS IS WHY.
        //
        // `mcpp::action{role = "source"}` ADDS its outputs to the compile set;
        // there is no mechanism to replace an input. So a package that both
        // globs `src/**/*.cpp` and receives a transformed copy links two
        // definitions of every composable. Letting the rule own the selection
        // -- ordinary sources through mcpp::source(), composable ones through
        // the transform -- is the only shape that yields one object per file.
        auto cg = plan_codegen(sources);
        // The SDK's installer page, from the directory it was rendered into.
        const auto interface_cg = plan_codegen(interface_sources, interface_root);
        cg.insert(cg.end(), interface_cg.begin(), interface_cg.end());
        std::vector<std::string> transformed;
        for (const edge& e : cg) {
            transformed.push_back(e.inputs.empty() ? std::string{} : e.inputs[0]);
        }
        std::vector<std::filesystem::path> units;
        for (const std::string& rel : sources) units.push_back(std::filesystem::path(mcpp::manifest_dir()) / rel);
        for (const std::string& rel : interface_sources) units.push_back(interface_root / rel);
        for (const std::filesystem::path& unit : units) {
            if (std::ranges::find(transformed, unit.string()) == transformed.end())
                mcpp::source(unit.string().c_str());
        }
        if (!cg.empty()) {
            detail::suppress_attribute_warning();
            // A transformed source is compiled from MCPP_OUT_DIR, so its own
            // relative #includes no longer resolve. CMake solves this with
            // target_include_directories(PRIVATE <source dir>); the same
            // directories have to be added here or the moved file stops
            // finding its neighbours.
            std::vector<std::string> seen;
            for (const std::filesystem::path& unit : units) {
                std::string s = unit.parent_path().string();
                if (std::ranges::find(seen, s) == seen.end()) {
                    seen.push_back(s);
                    mcpp::include_dir(s.c_str());
                }
            }
        }
        edges.insert(edges.end(), cg.begin(), cg.end());
    }

    // An application is a package with an entry; a library has none, and
    // deploys nothing (plan_resources says why). Nor does an installer
    // interface, whose resource package the application assembles.
    const bool application = !opt.bootstrapper && std::filesystem::is_regular_file(
        std::filesystem::path(mcpp::manifest_dir()) / opt.entry);
    auto rs = plan_resources(opt, application, has_module_interface);
    if (!rs.empty()) {
        mcpp::include_dir(
            (std::string(mcpp::out_dir()) + "/hrc/builtin/include").c_str());
    }
    // hrc writes the package's own accessor header -- `<namespace>_resources.h`,
    // the one huxerui_add_app()'s generated `app_resources.h` is on the CMake
    // path -- under `<output>/include/`, beside the package it describes.
    if (!opt.resources.empty()) {
        mcpp::include_dir(
            (std::string(mcpp::out_dir()) + "/hrc/app/include").c_str());
    }
    edges.insert(edges.end(), rs.begin(), rs.end());

    if (!submit(edges)) return false;

    if (opt.bootstrapper) return detail::windows_bootstrapper(opt, root);
    // A library provides no format: it has no launcher to package. The
    // formats themselves are huxerui.rules.dist's.
    if (!application) return true;
    // BUNDLE_NAME and BUNDLE_IDENTIFIER, stated once as huxerui_add_app()
    // takes them, name the `.app`, the APK and the Windows package alike; a
    // format's own field still wins.
    apple_options     apple     = opt.apple;
    android_options   android   = opt.android;
    installer_options installer = opt.installer;
    if (apple.display_name.empty())     apple.display_name     = opt.bundle_name;
    if (apple.bundle_id.empty())        apple.bundle_id        = opt.bundle_identifier;
    if (android.application_id.empty()) android.application_id = opt.bundle_identifier;
    if (installer.display_name.empty()) installer.display_name = opt.bundle_name;
    if (installer.project_id.empty())   installer.project_id   = opt.bundle_identifier;
    installer_interface setup_interface;
    if (std::string_view(mcpp::target_os()) == "windows" && std::string_view(mcpp::pack_format()) == "setup" &&
        !detail::plan_setup_interface(opt, installer, root, setup_interface))
        return false;
    return provide_formats({ opt.target, installer, opt.appimage, apple, opt.web, android, setup_interface },
                           root);
}

// The framework's own build needs only the builtin resource header: 11 of its
// translation units #include "huxerui_builtin_resources.h", so this cannot be
// deferred to the consumer even though the compiled package is also produced
// here and thrown away.
inline bool builtin_resources() {
    const std::string hrc = host_tool("hrc");
    if (hrc.empty()) {
        std::cerr << "huxerui.rules: hrc was not provided -- the `huxerui` "
                     "dependency must carry tools = [\"hrc\"]\n";
        return false;
    }
    const std::string odir = std::string(mcpp::out_dir()) + "/hrc";
    const std::string src  = std::string(mcpp::manifest_dir()) + "/resources";

    mcpp::rerun_if_changed_glob("resources/**");

    const std::string dep    = odir + "/builtin.d";
    const std::string header = odir + "/builtin/include/huxerui_builtin_resources.h";
    std::vector<std::string> outputs{ header };
    for (const std::string& p : huxerui::rules::sources::resource_outputs(src, "huxerui"))
        outputs.push_back(odir + "/builtin/package/" + p);

    edge e{
        .id          = "hrc:builtin",
        .role        = "source",
        .description = "huxerui builtin resources",
        .command     = { hrc, "--root", src,
                         "--output", odir + "/builtin",
                         "--namespace", "huxerui",
                         "--header-name", "huxerui_builtin_resources.h",
                         "--depfile", dep, "--depfile-target", header },
        .inputs      = { hrc },
        .outputs     = outputs,
        .depfile     = dep,
    };
    mcpp::include_dir((odir + "/builtin/include").c_str());
    return submit(std::span<const edge>(&e, 1));
}

} // namespace huxerui::rules
