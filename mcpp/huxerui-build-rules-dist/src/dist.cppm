// The distribution formats of a HuxerUI application built by mcpp -- msi and
// setup, appimage, app and dmg, web, apk and aab -- as `mcpp:plugins`' dist
// members configured the HuxerUI way. A host module the framework re-exports; `huxerui.rules`
// composes these options into its own and calls provide_formats() for an
// application.

export module huxerui.rules.dist;

import std;
import mcpp;
import huxerui.rules.sources;
import mcpp.dist.wix;
import mcpp.dist.appimage;
import mcpp.dist.apple;
import mcpp.dist.web;
import mcpp.dist.apk;

export namespace huxerui::rules {

// What an application may state about its Windows installer, and nothing it
// could have been asked for twice. The format itself is `mcpp:plugins`'
// `dist-wix`, given the WiX definitions CMake's Windows package renders
// (windows_definitions() below). Every field is optional: the version comes
// from [package], and the manufacturer and both upgrade codes from the
// project id, as CMake derives them -- a stable GUID chosen once per product,
// which is exactly what a derived value is. The formats are provided on every
// Windows build; these only change them.
struct installer_options {
    std::string target;         // the [targets.*] app to install; default: options::target
    std::string version;        // default: [package] version, made MSI-shaped
    std::string upgrade_code;   // the MSI's; default: derived from the project id
    std::string manufacturer;   // default: the project id
    std::string display_name;   // default: BUNDLE_NAME, else the package name
    // `@PROJECT_ID@` of CMake's Windows package: the manufacturer, the registry
    // key the shortcuts keep, and the name both upgrade codes are derived from.
    // Default: BUNDLE_IDENTIFIER; without either, dist-wix's derivations.
    std::string project_id;
    std::string icon;           // .ico, relative to the manifest; default: assets/app.ico, else the SDK template's
};

// THE INSTALLER INTERFACE SETUP.EXE RUNS, resolved by huxerui.rules before
// provide_formats() under `--format setup`: the program the application's
// `installer` package builds (a host tool of its Windows rows) and the files
// CMake stages beside it -- WiX's `mbanative.dll` and the interface's resource
// package.
struct installer_interface {
    std::string program;
    std::vector<std::pair<std::string, std::string>> payloads;  // (file, name beside the program)
};

// What an application may state about its AppImage; `dist-appimage` does the
// rest, with a placeholder icon when none is named. The Windows counterpart
// is installer_options above.
struct appimage_options {
    std::string target;                  // the [targets.*] app to package; default: options::target
    std::string display_name;            // default: the target name
    std::string icon;                    // a .png, relative to the manifest
    std::vector<std::string> categories; // freedesktop categories; default "Utility"
};

// What an application may state about its `.app` bundle on macOS and iOS;
// `dist-apple` does the rest. On macOS `icon` is one `.icns` file; on iOS it
// is a DIRECTORY of flat PNGs, which the member lists under CFBundleIcons.
struct apple_options {
    std::string target;         // the [targets.*] app to bundle; default: options::target
    std::string display_name;   // CFBundleName; default: the target name
    std::string bundle_id;      // CFBundleIdentifier; default: derived from namespace + name
    std::string icon;           // .icns (macOS) or a directory of .png (iOS), relative to the manifest
};

// The page `mcpp pack --format web` writes beside the launcher. Every HuxerUI
// application is a Web application when built for wasm32-emscripten, so the
// format is always provided; these only replace the page the rule ships.
struct web_options {
    std::string template_file;  // package-root-relative; `{{name}}` and `{{title}}` are substituted
    std::string title;          // <title>; default: the package name
};

// What an application may state about its APK; every field has a default that
// `dist-apk` or this rule derives, so a template application states nothing.
struct android_options {
    std::string application_id;     // manifest package; default: <namespace>.<name>
    std::string label;              // android:label; default: the package name
    std::string activity;           // the launcher Activity; default: org.huxerui.HuxerUIActivity
    std::string java;               // a directory of the application's own Java, relative to the manifest; default: android/java when present
    std::string res;                // an aapt2 `res/` directory, relative to the manifest; default: android/res when present, else the SDK's launcher icon set
    std::string manifest_template;  // replaces the manifest the rule ships, relative to the manifest
};

// What provide_formats() is given: the application's target and the five
// option sets `huxerui::rules::options` carries under the same names.
struct formats {
    std::string         target;
    installer_options   installer;
    appimage_options    appimage;
    apple_options       apple;
    web_options         web;
    android_options     android;
    installer_interface setup_interface;
};

namespace detail {

inline bool refuse(const std::string& message) {
    mcpp::warning(message.c_str());
    std::cerr << message << "\n";
    return false;
}

// THE WINDOWS PACKAGE CMAKE BUILDS, for `--format msi` and `--format setup`.
//
// huxerui_configure_windows_project_package() renders Package.wxs and
// Bundle.wxs from the SDK's Windows template; the MSI is a perMachine package
// in Program Files with a Start menu shortcut and an optional desktop one, and
// the bundle is `<target>-Setup-<version>.exe`, whose interface is the
// application's own installer program. The same two files are rendered here
// (huxerui::rules::sources::windows_package_definition says what differs and
// why) and handed to dist-wix, which builds them. Without these, dist-wix's
// generated MSI would install the program with no shortcut and its bundle
// would run WiX's stock interface -- a different installer for the same
// project depending on the build system.
inline bool windows_definitions(const formats& opt, const std::string& root,
                                mcpp::dist::wix::options& w, bool setup) {
    namespace sources = huxerui::rules::sources;
    const std::filesystem::path shell =
        std::filesystem::path(root) / "tools/huxerui_cli/templates/platform/windows/app";
    const std::filesystem::path manifest = mcpp::manifest_dir();
    const std::string package_name = mcpp::package_name();
    const std::string target = !w.target.empty() ? w.target : package_name;

    sources::windows_installer_values v;
    v.target_name  = target;
    v.project_name = !opt.installer.display_name.empty() ? opt.installer.display_name : package_name;
    const std::string raw_version =
        !opt.installer.version.empty() ? opt.installer.version : std::string(mcpp::package_version());
    const auto version = mcpp::dist::wix::msi_version_from(raw_version);
    if (!version.ok) {
        return refuse("huxerui.rules: '" + raw_version + "' is not a numeric, dot-separated version, which "
                      "an MSI requires; set installer_options::version");
    }
    v.version = version.text;
    const std::string identity = std::string(mcpp::package_namespace()) + "/" + package_name;
    v.project_id = !opt.installer.manufacturer.empty() ? opt.installer.manufacturer
                 : !opt.installer.project_id.empty()   ? opt.installer.project_id
                                                        : mcpp::dist::wix::manufacturer_for(w);
    v.msi_upgrade_code =
        !opt.installer.upgrade_code.empty() ? opt.installer.upgrade_code
        : !opt.installer.project_id.empty()
            ? sources::uuid_v5(sources::windows_upgrade_code_namespace, opt.installer.project_id + ".msi")
            : mcpp::dist::wix::upgrade_code_for(identity);
    v.bundle_upgrade_code =
        !opt.installer.project_id.empty()
            ? sources::uuid_v5(sources::windows_upgrade_code_namespace, opt.installer.project_id + ".bundle")
            : mcpp::dist::wix::upgrade_code_for(identity + "#bundle");

    std::filesystem::path icon = manifest / (opt.installer.icon.empty() ? std::string("assets/app.ico")
                                                                        : opt.installer.icon);
    if (!std::filesystem::is_regular_file(icon)) {
        if (!opt.installer.icon.empty())
            return refuse("huxerui.rules: the installer icon " + icon.string() + " was not found");
        icon = shell / "app.ico";
    }
    v.icon = icon.string();

    const std::filesystem::path out = std::filesystem::path(mcpp::out_dir()) / "huxerui-windows";
    const auto definition = [&](const char* name, const std::string& tmpl, bool bundle) -> std::string {
        if (tmpl.empty()) {
            refuse("huxerui.rules: the SDK's Windows template " + (shell / "package" / name).string() +
                   ".in was not found");
            return {};
        }
        const sources::rendered_definition r =
            bundle ? sources::windows_bundle_definition(tmpl, v) : sources::windows_package_definition(tmpl, v);
        if (!r.error.empty()) {
            refuse("huxerui.rules: " + r.error);
            return {};
        }
        const std::string path = (out / name).string();
        if (!mcpp::dist::wix::write_if_different(path, r.text)) {
            refuse("huxerui.rules: cannot write " + path);
            return {};
        }
        return path;
    };
    const auto read = [](const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    mcpp::rerun_if_changed((shell / "package/Package.wxs.in").string().c_str());
    w.wxs = definition("Package.wxs", read(shell / "package/Package.wxs.in"), false);
    if (w.wxs.empty()) return false;
    w.inputs = { v.icon };
    // `<target>.msi`, the name the bundle's chain carries under CMake.
    w.output = (out / (target + ".msi")).string();
    if (!setup) return true;

    v.interface_program = opt.setup_interface.program;
    v.payloads          = opt.setup_interface.payloads;
    mcpp::rerun_if_changed((shell / "package/Bundle.wxs.in").string().c_str());
    w.bundle_wxs = definition("Bundle.wxs", read(shell / "package/Bundle.wxs.in"), true);
    if (w.bundle_wxs.empty()) return false;
    w.bundle_inputs = { v.icon, v.interface_program };
    for (const auto& payload : v.payloads) w.bundle_inputs.push_back(payload.first);
    w.bundle_output = (out / (target + "-Setup-" + raw_version + ".exe")).string();
    return true;
}

} // namespace detail

// ------------------------------------------------------------ dist members --
// Every format the row serves, provided for `mcpp pack --format <name>` with
// nothing stated; the options only change what a format produces. `root` is
// the HuxerUI SDK root, where the page template, the Android manifest and the
// launcher icon set the SDK ships live.
inline bool provide_formats(const formats& opt, const std::string& root) {
    // The distribution formats are `mcpp:plugins`' dist members, reached
    // through this package's own build-dependency. Each is provided on the
    // row it serves and nowhere else, so `mcpp pack --format apk` on a Linux
    // desktop build is an unknown format rather than a declined one; a
    // member's plan is a no-op until `mcpp pack` names its format, so this
    // costs an ordinary build nothing. The payloads they run (xim:wix,
    // xim:appimagetool, the NDK's build tools, ...) are declared by the
    // members themselves, and a host module's declaration reaches every
    // build program it is compiled into -- which is why an application
    // declares none of them.
    const std::string dist_os  = mcpp::target_os();
    const std::string dist_env = mcpp::target_env();
    const auto target_or = [&](const std::string& named) { return named.empty() ? opt.target : named; };
    if (dist_os == "windows") {
        mcpp::dist::wix::options w;
        w.target       = target_or(opt.installer.target);
        w.product_name = opt.installer.display_name;
        w.manufacturer = opt.installer.manufacturer;
        w.version      = opt.installer.version;
        w.upgrade_code = opt.installer.upgrade_code;
        // HuxerUI's own definitions, only when a Windows format is being
        // packed: every other build only declares the formats.
        const std::string format = mcpp::pack_format();
        if ((format == "msi" || format == "setup") &&
            !detail::windows_definitions(opt, root, w, format == "setup"))
            return false;
        if (!mcpp::dist::wix::generate(w)) return false;
    }
    if (dist_os == "linux" && dist_env != "android") {
        mcpp::dist::appimage::options a;
        a.target     = target_or(opt.appimage.target);
        a.app_name   = opt.appimage.display_name;
        a.icon       = opt.appimage.icon;
        a.categories = opt.appimage.categories;
        a.terminal   = false;
        if (!mcpp::dist::appimage::generate(a)) return false;
    }
    if (dist_os == "macos" || dist_os == "ios") {
        mcpp::dist::apple::options a;
        a.target    = target_or(opt.apple.target);
        a.app_name  = opt.apple.display_name;
        a.bundle_id = opt.apple.bundle_id;
        a.icon      = opt.apple.icon;
        // `generate` also links the program with the rpath that finds
        // `Contents/Frameworks/` and, on macOS, supplies the runner named
        // `app` (`macapp-run`), so `mcpp run --format app` runs the bundle in
        // the foreground and returns the program's status.
        if (!mcpp::dist::apple::generate(a)) return false;
    }
    if (dist_os == "ios" && dist_env == "sim") {
        // The simulator runs the program the host cannot: `simctl-run`
        // installs a bundle and returns the program's status. Supplied here so
        // an application's manifest names no runner; the payload that carries
        // it is declared in this package's manifest for `mcpp run` only.
        mcpp::runner("simctl-run");
    }
    if (dist_os == "emscripten") {
        // The page the rule ships calls the MODULARIZE factory
        // the emscripten section exports and mounts the application; a
        // project supplies its own to change the page, not the contract.
        // `dist-web` joins the template path to the manifest directory, and
        // an absolute path survives that join, which is how the rule's own
        // template -- in the SDK, not the project -- reaches it.
        mcpp::dist::web::options w;
        w.target        = opt.target;
        w.template_file = opt.web.template_file.empty()
            ? root + "/mcpp/huxerui-build-rules-dist/web/index.html.in" : opt.web.template_file;
        w.title         = opt.web.title;
        if (!mcpp::dist::web::generate(w)) return false;
    }
    if (dist_env == "android") {
        // Level 1 of dist-apk with the framework's Java host as the first
        // root and, when present, the
        // application's own as the second; the Activity is the framework's
        // unless the application names its own subclass.
        const std::string manifest_dir = mcpp::manifest_dir();
        const auto under_manifest = [&](const std::string& rel) {
            return (std::filesystem::path(manifest_dir) / rel).string();
        };
        mcpp::dist::apk::options a;
        a.target         = opt.target;
        a.application_id = opt.android.application_id;
        a.label          = opt.android.label;
        a.java_sources   = { root + "/platform/android/huxerui/src/main/java" };
        const std::string java = opt.android.java.empty() ? std::string("android/java") : opt.android.java;
        if (std::filesystem::is_directory(under_manifest(java))) a.java_sources.push_back(under_manifest(java));
        if (!opt.android.activity.empty()) {
            a.activity = opt.android.activity;
        } else {
            // THE LAUNCHER ACTIVITY IS THE APPLICATION'S OWN SUBCLASS, as in a
            // Gradle project: `<application id>.MainActivity extends
            // HuxerUIActivity`, whose static initialiser loads the
            // application's shared object -- the one thing the framework's
            // Activity cannot know the name of. The rule writes that class
            // from the same two values the manifest carries, so the
            // framework's Java is identical under both build systems.
            std::string app_id = !opt.android.application_id.empty() ? opt.android.application_id
                : (std::string(mcpp::package_namespace()).empty() ? std::string("app")
                                                                   : std::string(mcpp::package_namespace()))
                  + "." + std::string(mcpp::package_name());
            // dist-apk's own default, which a Java package can carry: no dashes.
            if (opt.android.application_id.empty())
                for (std::size_t i = 0; i < app_id.size(); ++i) if (app_id[i] == '-') app_id[i] = '_';
            const std::string lib_name = opt.target.empty() ? std::string(mcpp::package_name()) : opt.target;
            std::string package_dir = app_id;
            for (char& c : package_dir) if (c == '.') c = '/';
            const std::filesystem::path generated_root = std::filesystem::path(mcpp::out_dir()) / "android" / "java";
            const std::filesystem::path source = generated_root / package_dir / "MainActivity.java";
            std::error_code ec;
            std::filesystem::create_directories(source.parent_path(), ec);
            std::ofstream file(source, std::ios::binary | std::ios::trunc);
            if (!file) { std::cerr << "huxerui.rules: cannot write " << source.string() << "\n"; return false; }
            file << "package " << app_id << ";\n\n"
                    "import org.huxerui.HuxerUIActivity;\n\n"
                    "/** Generated by huxerui.rules: loads the application's shared object, as a Gradle project's MainActivity does. */\n"
                    "public final class MainActivity extends HuxerUIActivity {\n"
                    "    static {\n"
                    "        System.loadLibrary(\"" << lib_name << "\");\n"
                    "    }\n"
                    "}\n";
            file.close();
            a.java_sources.push_back(generated_root.string());
            // The manifest's package is the class's package by construction.
            a.application_id = app_id;
            a.activity = app_id + ".MainActivity";
        }
        // The application's res/ when it has one, else the launcher icon set
        // the SDK's Gradle template ships -- one icon in the repository, and
        // every APK has one. An application's own res/ replaces it whole, so
        // it carries `@mipmap/ic_launcher`, which the manifest names.
        const std::string res = opt.android.res.empty() ? std::string("android/res") : opt.android.res;
        a.resources = std::filesystem::is_directory(under_manifest(res))
            ? under_manifest(res)
            : root + "/tools/huxerui_cli/templates/platform/android/app/app/src/main/res";
        // The framework's Java root is a dependency's, so its rerun glob would
        // match nothing; the list the rule package carries is what re-runs
        // this program when a host file is added or removed.
        mcpp::rerun_if_changed((root + "/mcpp/huxerui-build-rules-dist/android/java-sources.txt").c_str());
        // dist-apk joins the template to the manifest directory; an absolute
        // path survives the join, which is how the SDK's own reaches it.
        a.manifest_template = opt.android.manifest_template.empty()
            ? root + "/mcpp/huxerui-build-rules-dist/android/AndroidManifest.xml.in"
            : under_manifest(opt.android.manifest_template);
        if (!mcpp::dist::apk::generate(a)) return false;
        // The device or emulator runs what the host cannot: `adb-run` installs
        // an APK (or pushes a program), follows its log and returns its status.
        // Supplied here so an application's manifest names no runner.
        mcpp::runner("adb-run");
    }
    return true;
}

} // namespace huxerui::rules
