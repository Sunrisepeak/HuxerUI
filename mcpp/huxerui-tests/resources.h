// The resource packages the mcpp test suites open, compiled as CMake's test
// targets compile them: the framework's builtin package (the one
// tests/support/builtin_resources.cpp reads) and a suite's fixture merged with
// it (huxerui_add_resources() plus the builtin input). Included by the build
// programs of this directory after `import std;`, `import mcpp;` and
// `import huxerui.rules;`.

namespace huxerui_tests {

// A resource tree's files, which are the inputs of the edge that compiles it.
inline std::vector<std::string> tree_inputs(const std::string& hrc, const std::string& tree) {
    std::vector<std::string> inputs{ hrc };
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(tree, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file(ec)) inputs.push_back(it->path().string());
    }
    return inputs;
}

// FORWARD SLASHES, and not for tidiness. These paths are baked into string
// literals by the -D a suite emits, and on Windows out_dir() is a backslash
// path: `...\out\hrc\...` puts `\o` and `\h` in the literal, and clang reports
// `expected '{' after '\o' escape sequence`. Windows accepts '/' everywhere.
inline std::string forward(const std::string& path) {
    return std::filesystem::path(path).generic_string();
}

// hrc over `tree` under `name`, into <out_dir>/hrc/<name>; returns the package.
inline std::string compile_package(const std::string& hrc, const std::string& tree, const std::string& name,
                                   const std::string& header) {
    const std::string out = std::string(mcpp::out_dir()) + "/hrc/" + name;
    const std::string package = forward(out + "/package");
    // `id` and `description` are pointers the action keeps until submit(), so
    // the strings they point into live to the end of this function.
    const std::string id = "hrc:" + name;
    const std::string description = "resources " + name;
    mcpp::action a;
    a.id          = id.c_str();
    a.role        = "source";
    a.description = description.c_str();
    a.arg(hrc.c_str());
    a.arg("--root");        a.arg(tree.c_str());
    a.arg("--output");      a.arg(out.c_str());
    a.arg("--namespace");   a.arg(name.c_str());
    a.arg("--header-name"); a.arg(header.c_str());
    for (const std::string& i : tree_inputs(hrc, tree)) a.input(i.c_str());
    a.output((out + "/include/" + header).c_str());
    a.output((package + "/huxerui/resources.bin").c_str());
    a.submit();
    mcpp::include_dir((out + "/include").c_str());
    return package;
}

// The framework's builtin package, namespace `huxerui`.
inline std::string builtin_package(const std::string& root, const std::string& hrc) {
    return compile_package(hrc, root + "/resources", "huxerui", "huxerui_builtin_resources.h");
}

// A fixture tree compiled under `name` and merged with the builtin package into
// the one directory the suite opens; returns that directory.
inline std::string fixture_package(const std::string& root, const std::string& hrc, const std::string& tree,
                                   const std::string& name) {
    const std::string builtin = builtin_package(root, hrc);
    const std::string fixture = compile_package(hrc, tree, name, name + "_resources.h");
    const std::string out = std::string(mcpp::out_dir()) + "/hrc/" + name + "-merged";
    const std::string merged = forward(out + "/package");
    const std::string id = "hrc:" + name + "-merge";
    const std::string description = "resources " + name + " with the builtin package";
    mcpp::action m;
    m.id          = id.c_str();
    m.role        = "source";
    m.description = description.c_str();
    m.arg(hrc.c_str()); m.arg("merge");
    m.arg("--input");  m.arg(fixture.c_str());
    m.arg("--input");  m.arg(builtin.c_str());
    m.arg("--output"); m.arg(out.c_str());
    m.input(hrc.c_str());
    m.input((fixture + "/huxerui/resources.bin").c_str());
    m.input((builtin + "/huxerui/resources.bin").c_str());
    m.output((merged + "/huxerui/resources.bin").c_str());
    m.submit();
    return merged;
}

// A string-literal definition of a path. The quotes are escaped because ninja
// runs the compile line through a shell, which eats an unescaped pair -- CMake
// escapes the same definitions for the same reason.
inline void define_path(const std::string& name, const std::string& path) {
    mcpp::cxxflag(("-D" + name + "=\\\"" + path + "\\\"").c_str());
}

// The SDK root and hrc, which every build program here needs first.
inline bool locate(std::string& root, std::string& hrc) {
    root = huxerui::rules::sdk_root();
    hrc  = huxerui::rules::host_tool("hrc");
    if (root.empty() || hrc.empty()) {
        std::cerr << "huxerui-tests: the HuxerUI root or hrc was not provided by the huxerui dependency\n";
        return false;
    }
    return true;
}

} // namespace huxerui_tests
