// The distribution formats of a HuxerUI application built by mcpp -- msi and
// setup, appimage, app and dmg, web, apk and aab -- as `mcpp:plugins`' dist
// members configured the HuxerUI way. A host module the framework re-exports; `huxerui.rules`
// composes these options into its own and calls provide_formats() for an
// application.

export module huxerui.rules.dist;

import std;
import mcpp;
import mcpp.plugins;
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
    std::string display_name;   // CFBundleName and CFBundleDisplayName; default: the target name
    std::string bundle_id;      // CFBundleIdentifier; default: derived from namespace + name
    std::string icon;           // .icns (macOS) or a directory of .png (iOS), relative to the manifest
    // THE APPLICATION'S OWN Info.plist ENTRIES, a plist relative to the manifest -- usage descriptions, URL
    // types, background modes, what an Xcode project's Info.plist states. Default: `ios/Info.plist` on iOS and
    // `macos/Info.plist` on macOS, when present. Merged over the entries CMake's application template writes
    // (template_info_plist() below); a key the bundle derives -- the identifier, the versions, the executable --
    // is refused by name.
    std::string info_plist;
    // SIGNING, stated explicitly, as an Xcode project's signing settings are. `identity` is a keychain
    // identity: without one a macOS bundle is signed ad hoc and an iOS device bundle is not signed.
    // `entitlements` is a plist relative to the manifest. `provisioning_profile`, relative to the manifest, is
    // embedded in an iOS device bundle and supplies its entitlements when `entitlements` is empty; it needs
    // `identity`.
    std::string identity;
    std::string entitlements;
    std::string provisioning_profile;
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
    // A directory of the application's own Kotlin, relative to the manifest; default: android/kotlin when
    // present. Compiled by `xim:kotlin`, which the framework's `android-kotlin` feature brings:
    //     huxerui.huxerui = { ..., features = ["android-kotlin"] }
    std::string kotlin;
    std::string res;                // an aapt2 `res/` directory, relative to the manifest; default: android/res when present, else the SDK's launcher icon set
    // A directory of `.jar` and `.aar` files the application uses, relative to the manifest; default:
    // android/libs when present.
    std::string libs;
    // Maven coordinates (`group:artifact:version`), resolved with their transitive dependencies into
    // `maven_lock` (relative to the manifest; default: android/maven.lock), which the project commits. An
    // ordinary build reads the locked artifacts from the cache and does not reach the network;
    // `MCPP_DIST_APK_MAVEN=update` resolves, `=fetch` downloads, with `xim:coursier` from the framework's
    // `android-maven` feature. Empty repositories mean Google's Maven repository, then Maven Central.
    std::vector<std::string> maven;
    std::string              maven_lock;
    std::vector<std::string> maven_repositories;
    std::string manifest_template;  // replaces the manifest the rule ships, relative to the manifest
    // SIGNING, stated explicitly, as a Gradle project's signing configuration is. With a keystore (a package
    // name, `ns:name`, whose payload holds the key) every APK is signed with it; `keystore_password_env` names
    // the variable the tools read the password from. Without one a debug build is signed with the Android debug
    // key and a release build is not signed, as Gradle's release variant without a signing configuration.
    std::string keystore;
    std::string keystore_alias;
    std::string keystore_password_env;
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

inline std::string read_text(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline bool write_text_if_different(const std::filesystem::path& p, const std::string& text) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(p, ec) && read_text(p) == text) return true;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out);
}

} // namespace detail

// THE HUXERUI LIBRARIES AN APPLICATION DEPENDS ON DIRECTLY: the dependencies
// its manifest declares (`mcpp::dep_dir` answers for exactly those) whose own
// manifest depends on `huxerui.huxerui`, in the order the manifest writes them.
// What CMake's huxerui_use_library() calls name. A library reached only through
// another dependency is not visible to a build program (mcpp-community/mcpp#647,
// E1).
struct direct_library {
    std::string                                  key;
    std::filesystem::path                        root;
    huxerui::rules::sources::dependency_manifest manifest;
};

inline std::vector<direct_library> direct_libraries() {
    std::vector<direct_library> out;
    const std::filesystem::path manifest = std::filesystem::path(mcpp::manifest_dir()) / "mcpp.toml";
    std::vector<std::filesystem::path> seen;
    for (const std::string& key : huxerui::rules::sources::dependency_keys(detail::read_text(manifest))) {
        const std::string dir = mcpp::dep_dir(key.c_str());
        if (dir.empty()) continue;
        std::error_code ec;
        const std::filesystem::path root = std::filesystem::weakly_canonical(dir, ec);
        if (ec || std::ranges::find(seen, root) != seen.end()) continue;
        seen.push_back(root);
        auto library = huxerui::rules::sources::read_dependency_manifest(detail::read_text(root / "mcpp.toml"));
        if (!library.uses_huxerui) continue;
        out.push_back({ key, root, std::move(library) });
    }
    return out;
}

namespace detail {

// THE Info.plist ENTRIES CMAKE'S APPLICATION TEMPLATES WRITE beyond the ones
// dist-apple derives (tools/huxerui_cli/templates/platform/{ios,macos}/app):
// the display name on both; on iOS the development region, the dictionary
// version, the supported orientations, and a launch screen. Without a launch
// screen iOS runs an application in compatibility mode rather than at the
// device's resolution. The template's `LaunchScreen.storyboard` is a blank
// `systemBackgroundColor` view, which an empty `UILaunchScreen` dictionary is
// too, with no storyboard to compile (Xcode's `ibtool` is not redistributable).
inline std::string template_info_plist(bool ios, const std::string& display_name) {
    std::string entries = "<key>CFBundleDisplayName</key><string>" + display_name + "</string>";
    if (ios) {
        entries += "<key>CFBundleDevelopmentRegion</key><string>en</string>"
                   "<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>"
                   "<key>UILaunchScreen</key><dict/>"
                   "<key>UISupportedInterfaceOrientations</key><array>"
                   "<string>UIInterfaceOrientationPortrait</string>"
                   "<string>UIInterfaceOrientationLandscapeLeft</string>"
                   "<string>UIInterfaceOrientationLandscapeRight</string></array>"
                   "<key>UISupportedInterfaceOrientations~ipad</key><array>"
                   "<string>UIInterfaceOrientationPortrait</string>"
                   "<string>UIInterfaceOrientationPortraitUpsideDown</string>"
                   "<string>UIInterfaceOrientationLandscapeLeft</string>"
                   "<string>UIInterfaceOrientationLandscapeRight</string></array>";
    }
    return "<plist version=\"1.0\"><dict>" + entries + "</dict></plist>";
}

// The template's entries with the application's own merged over them -- a key
// both state is the application's -- written as one plist for dist-apple's
// `info_plist`, which refuses a key the bundle derives.
inline bool write_info_plist(const std::string& template_plist, const std::string& application_plist,
                             const std::filesystem::path& out, std::string& error) {
    namespace xml = mcpp::plugins::xml;
    const auto top_dict = [](xml::node& root) -> xml::node* {
        if (root.name == "dict") return &root;
        for (xml::node& child : root.children) if (child.name == "dict") return &child;
        return nullptr;
    };
    const auto key_of = [](const xml::node& n) {
        return n.name == "key" && n.children.size() == 1 && n.children.front().name.empty()
            ? xml::trim_copy(n.children.front().text) : std::string();
    };
    xml::node merged;
    if (!xml::parse(template_plist, merged, error)) return false;
    xml::node* into = top_dict(merged);
    if (!into) { error = "the template's entries are not a dictionary"; return false; }
    if (!application_plist.empty()) {
        xml::node app;
        if (!xml::parse(read_text(application_plist), app, error)) {
            error = application_plist + ": " + error;
            return false;
        }
        xml::node* from = top_dict(app);
        if (!from) { error = application_plist + " has no top-level <dict>"; return false; }
        for (std::size_t i = 0; i + 1 < from->children.size(); i += 2) {
            const std::string key = key_of(from->children[i]);
            if (key.empty()) { error = application_plist + ": a <dict> entry does not open with a <key>"; return false; }
            for (std::size_t j = 0; j + 1 < into->children.size(); j += 2) {
                if (key_of(into->children[j]) == key) {
                    into->children.erase(into->children.begin() + static_cast<std::ptrdiff_t>(j),
                                         into->children.begin() + static_cast<std::ptrdiff_t>(j + 2));
                    break;
                }
            }
            into->children.push_back(from->children[i]);
            into->children.push_back(from->children[i + 1]);
        }
    }
    std::string text = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                       "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                       "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n";
    xml::write(merged, text, 0);
    if (!write_text_if_different(out, text)) { error = "cannot write " + out.string(); return false; }
    return true;
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
        const std::filesystem::path manifest = mcpp::manifest_dir();
        const auto under_manifest = [&](const std::string& rel) {
            return rel.empty() ? std::string() : (manifest / rel).string();
        };
        mcpp::dist::apple::options a;
        a.target    = target_or(opt.apple.target);
        a.app_name  = opt.apple.display_name;
        a.bundle_id = opt.apple.bundle_id;
        a.icon      = opt.apple.icon;
        a.identity             = opt.apple.identity;
        a.entitlements         = under_manifest(opt.apple.entitlements);
        a.provisioning_profile = under_manifest(opt.apple.provisioning_profile);
        // THE Info.plist: the entries CMake's template writes, with the
        // application's own merged over them. Written only when this build
        // packs a bundle; every other build only declares the formats.
        const std::string format = mcpp::pack_format();
        if (format == "app" || format == "dmg") {
            const bool ios = dist_os == "ios";
            std::string application_plist = under_manifest(opt.apple.info_plist);
            if (application_plist.empty()) {
                const std::filesystem::path conventional = manifest / (ios ? "ios/Info.plist" : "macos/Info.plist");
                if (std::filesystem::is_regular_file(conventional)) application_plist = conventional.string();
            }
            if (!application_plist.empty()) {
                if (!std::filesystem::is_regular_file(application_plist))
                    return detail::refuse("huxerui.rules: the Info.plist " + application_plist + " was not found");
                mcpp::rerun_if_changed(application_plist.c_str());
            }
            const std::string display = !opt.apple.display_name.empty() ? opt.apple.display_name : a.target;
            const std::filesystem::path merged =
                std::filesystem::path(mcpp::out_dir()) / "huxerui-apple" / (ios ? "ios-Info.plist" : "macos-Info.plist");
            std::string error;
            if (!detail::write_info_plist(detail::template_info_plist(ios, display), application_plist, merged, error))
                return detail::refuse("huxerui.rules: " + error);
            a.info_plist = merged.string();
        }
        // `generate` also links the program with the rpath that finds
        // `Contents/Frameworks/` and supplies the runner named `app`:
        // `macapp-run` on macOS, which runs the bundle in the foreground and
        // returns the program's status, and `devicectl-run` on the iOS device
        // row, which installs a signed bundle and launches it.
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

        // THE APPLICATION'S KOTLIN, beside its Java, when it has any. The
        // compiler is `xim:kotlin`, which only the framework's `android-kotlin`
        // feature declares, so an application without Kotlin downloads none; a
        // Kotlin root without it is refused here, naming the feature to add.
        const std::string kotlin = opt.android.kotlin.empty() ? std::string("android/kotlin") : opt.android.kotlin;
        if (std::filesystem::is_directory(under_manifest(kotlin))) {
            if (std::string(mcpp::xpkg_dir("xim", "kotlin")).empty())
                return detail::refuse("huxerui.rules: " + under_manifest(kotlin) + " holds Kotlin, which is compiled "
                                      "by xim:kotlin; request it with the framework's `android-kotlin` feature:\n"
                                      "  huxerui.huxerui = { ..., features = [\"android-kotlin\"] }");
            a.kotlin_sources.push_back(under_manifest(kotlin));
        } else if (!opt.android.kotlin.empty()) {
            return detail::refuse("huxerui.rules: android_options::kotlin names " + under_manifest(kotlin) +
                                  ", which is not a directory");
        }

        // JARs and AARs: the application's own directory, then each library's.
        const auto archives_in = [&](const std::filesystem::path& dir) {
            std::vector<std::string> found;
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) return found;
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
                if (entry.is_regular_file(ec)) found.push_back(entry.path().string());
            std::ranges::sort(found);
            for (const std::string& f : found) {
                if (f.ends_with(".jar")) a.jars.push_back(f);
                else if (f.ends_with(".aar")) a.aars.push_back(f);
            }
            return found;
        };
        const std::string libs = opt.android.libs.empty() ? std::string("android/libs") : opt.android.libs;
        if (!opt.android.libs.empty() && !std::filesystem::is_directory(under_manifest(libs)))
            return detail::refuse("huxerui.rules: android_options::libs names " + under_manifest(libs) +
                                  ", which is not a directory");
        archives_in(under_manifest(libs));

        // THE LIBRARIES' ANDROID CONTRIBUTIONS, as a Gradle library module
        // makes them: Java and Kotlin, `res/` under the library's own R
        // package, a manifest merged into the application's, and assets.
        for (const direct_library& library : direct_libraries()) {
            const std::filesystem::path android = library.root / "android";
            if (!std::filesystem::is_directory(android)) continue;
            mcpp::dist::apk::library contribution;
            const std::filesystem::path library_manifest = android / "AndroidManifest.xml";
            std::string package;
            if (std::filesystem::is_regular_file(library_manifest)) {
                contribution.manifest = library_manifest.string();
                mcpp::plugins::xml::node node;
                std::string error;
                if (mcpp::plugins::xml::parse(detail::read_text(library_manifest), node, error))
                    package = mcpp::plugins::xml::attr_of(node, "package");
            }
            if (package.empty()) {
                const std::string ns = library.manifest.package_namespace;
                package = (ns.empty() || ns == "mcpplibs" ? std::string() : ns + ".") + library.manifest.name;
                for (char& c : package) if (c == '-') c = '_';
            }
            contribution.package = package;
            if (std::filesystem::is_directory(android / "res"))    contribution.resources = (android / "res").string();
            if (std::filesystem::is_directory(android / "assets")) contribution.assets = (android / "assets").string();
            if (std::filesystem::is_directory(android / "java"))   contribution.java_sources = { (android / "java").string() };
            if (std::filesystem::is_directory(android / "kotlin")) {
                if (std::string(mcpp::xpkg_dir("xim", "kotlin")).empty())
                    return detail::refuse("huxerui.rules: the library " + library.key + " holds Kotlin in " +
                                          (android / "kotlin").string() + ", which is compiled by xim:kotlin; "
                                          "request it with the framework's `android-kotlin` feature");
                contribution.kotlin_sources = { (android / "kotlin").string() };
            }
            archives_in(android / "libs");
            if (!contribution.manifest.empty() || !contribution.resources.empty() || !contribution.assets.empty() ||
                !contribution.java_sources.empty() || !contribution.kotlin_sources.empty())
                a.libraries.push_back(std::move(contribution));
        }

        // MAVEN, through the lock the project commits (see android_options::maven).
        a.maven              = opt.android.maven;
        a.maven_repositories = opt.android.maven_repositories;
        if (!a.maven.empty())
            a.maven_lock = under_manifest(opt.android.maven_lock.empty() ? std::string("android/maven.lock")
                                                                         : opt.android.maven_lock);

        // SIGNING AS GRADLE SIGNS: a stated keystore always; otherwise the
        // debug key for a debug build and nothing for a release one, which a
        // Gradle release variant without a signing configuration produces.
        a.keystore              = opt.android.keystore;
        a.keystore_alias        = opt.android.keystore_alias;
        a.keystore_password_env = opt.android.keystore_password_env;
        a.sign = !opt.android.keystore.empty() || std::string_view(mcpp::profile()) != "release";
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
