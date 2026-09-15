# mcpp Build System Design

HuxerUI has two build systems. This document specifies the second one: how the
framework is an [mcpp](https://github.com/mcpp-community/mcpp) package, how an
application consumes it, and what the C++20 module surface guarantees.

CMake remains the path of the release SDK and of the Gradle and Xcode shells.
Nothing in the CMake build reads any file described here, and deleting
`mcpp.toml`, `build.mcpp` and `mcpp/` returns the repository to a CMake-only
project.

What the two build systems must agree on -- the default program model, the
extension points a developer has under both, and the differences allowed
between them -- is the [Build Systems Specification](build-systems-spec.md).
This document says how mcpp meets it.

How to use it -- the environment, the commands per platform, and what mcpp adds
to the CMake build -- is
[C++20/23 Modules and mcpp: Six Platforms](../guide/cpp-modules-and-mcpp.md);
this document keeps the reasons.

## 1. Division of ownership

The program the two produce is the specification's default program model
(its §2). How each produces it differs:

| | CMake | mcpp |
|---|---|---|
| Platforms | Linux, Windows, macOS, Android, iOS, Web | Linux, Windows, macOS, Android, iOS, Web |
| Distribution | Gradle, Xcode, WiX bootstrapper, SDK archives | `mcpp pack --format setup\|msi\|appimage\|dmg\|app\|apk\|web` |
| Consumption | `#include <huxerui/huxerui.h>` | `import huxerui;` *and* the headers |
| Linux dependencies | the distribution's packages | xlings payloads |
| Toolchain | the host's | mcpp's own, per target |
| SDK packaging | `sdk-release.yml` | `mcpp pack` |
| Framework linkage | `_huxerui_select_framework_target`; Gradle sets `HUXERUI_BUILD_SHARED=ON` | stated by the framework per row: `[target.'cfg(env = "android")'.targets.huxerui] kind = "shared"` |
| Resource packages | `huxerui_add_resources`, merged from the CMake graph | `configure()`'s hrc edges, merged from the application's direct dependencies (§3) |
| Application identity | `huxerui_add_app(… BUNDLE_NAME BUNDLE_IDENTIFIER)` | `configure({ .bundle_name, .bundle_identifier })`, taken by the `.app`, the APK, the Web page and the Windows package |
| Platform metadata | the Gradle, Xcode and WiX templates | the same templates' entries, rendered by the rule (§7) |
| Platform floors | the templates | the templates state them, the framework's `version-floor` requirements refuse lower |
| Compile-time profiling | on in the repository's own build, off in the SDK applications build against | the `profiling` feature, off by default; the repository's examples and suites turn it on |
| Windows installer interface | `huxerui_add_windows_installer()`, built for a package build | the application's `windows/installer` package, a tool built only when the `windows-installer` feature is named, which `huxerui package windows` does |

The two answer *where does GTK come from* differently on purpose; §5 explains
why they must, and the specification lists what that changes in the AppImage.
Everything else follows one rule: **on the same platform the mcpp path takes the
CMake path's conventions by default.** A departure is an explicit option, never
a consequence of having chosen a build system: a shared framework on the
desktop is `linkage = "shared"` in the application's manifest (CI packs one on
macOS), and a statically linked framework on Android waits for a build
configuration that decides the artifact and the Java host's loading entry
together (§9). What a test needs (a smoke that exits, a first-frame condition)
lives on the test side, not in the runtime.

## 2. Package layout

```
mcpp.toml                     the framework as a package
build.mcpp                    its build program
modules/huxerui.cppm          the C++20 module interface (generated, committed)
mcpp/
  huxerui-build-rules/        `import huxerui.rules;` -- the application-facing API
  huxerui-source-select/      the scanning the rules and the tools share
  huxerui-tools/              huxerui-build-check, huxerui-module-gen
  huxerui-tests/              CMake's suites -- unit, runtime, ui, smoke -- one package each
  examples/                   three applications
templates/                    mcpp package templates: app, navigation, library
                              (an application's windows/installer is a package too)
tools/codegen/mcpp.toml       hcg as a package
tools/resource_compiler/…     hrc as a package
```

`huxerui-build-rules` is deliberately not a workspace member: it imports mcpp,
so it compiles only inside a build program's compile.

## 3. The application surface

An application's manifest names one dependency and its build program is one
call:

```toml
[dependencies]
huxerui.huxerui = "0.3.0"

[build]
sources = []
```

```cpp
import mcpp;
import huxerui.rules;

int main() { return huxerui::rules::configure({ .resources = "resources" }) ? 0 : 1; }
```

Everything else arrives through that edge, the framework's form on each row
included (§1). The framework declares its tools and its rule module as
**build**-dependencies with `reexport = true`:

```toml
[build-dependencies]
huxerui.huxerui-codegen           = { path = "tools/codegen",            tools = ["hcg"], reexport = true }
huxerui.huxerui-resource-compiler = { path = "tools/resource_compiler",  tools = ["hrc"], reexport = true }
huxerui.huxerui-build-rules       = { path = "mcpp/huxerui-build-rules", host-module = true, reexport = true }
```

`reexport` is what carries them one dependency edge further, so the application
reaches `huxerui.rules`, `hcg` and `hrc` without naming any of them. None of
the three reaches the artifact: two run on the build machine and produce
sources, and the third is compiled into `build.mcpp` itself.

### `sources = []` is not an oversight

`huxerui.rules` owns the source selection. A build program can **add** a source
but cannot replace one, so a package that also globbed `src/**/*.cpp` would
link both the original and the transformed copy of every composable —
`multiple definition of Counter()`. The rule selects, transforms and submits;
the manifest stays out of it.

The target's entry is the one file the rule never transforms, for the same
reason: a transformed copy of `main.cpp` would link beside the original.

### A library's resources join the application's package

A library states `.resources` in its own build program, as an application does,
and nothing in the application names it. The application's rule reads its own
manifest's dependency keys (`[dependencies]`, `[target.*.dependencies]`), asks
`mcpp::dep_dir` where each resolved, and takes the ones whose manifest depends
on HuxerUI and whose directory has the resources a HuxerUI library states. Each
is compiled under its namespace -- `package_product`, lower-cased, one segment
when the two are equal, and mcpp's default `mcpplibs` namespace is no segment --
and merged after the framework's builtin resources and before the
application's, in declaration order. That is the package CMake merges, with the
names `huxerui create library` derives. The same directories carry a library's
Android contributions (§7). Only direct dependencies are seen: a build program
has no view of the resolved graph beyond them
([mcpp#647](https://github.com/mcpp-community/mcpp/issues/647), E1).

## 4. The module surface

`modules/huxerui.cppm` includes the public headers in its global module
fragment and re-exports their names, so `import huxerui;` and
`#include <huxerui/huxerui.h>` name the **same entities** with the same
linkage. It is generated by `huxerui-module-gen` and committed; CI asserts it
is current, which is what catches a public name added to a header and not to
the shell.

### No headers in an application

An application built by mcpp contains no `#include` at all:

```cpp
export module app;

import std;
import huxerui;

[[huxerui::composable]]
View Counter() { auto count = UseState(0); … }
```

`import std;` is load-bearing. `UseState()`, `View` and `Layout` instantiate
`typeid` in their **caller**, GCC checks that per translation unit, and a
global module fragment's includes do not reach whoever imports it — so
`import huxerui;` cannot supply `<typeinfo>` the way the umbrella header does.
`huxerui.rules` cannot force it either: `-include` prepends before `module;`,
which is ill-formed. Importing `std` answers it without a header.

The std module is a C++23 *library* feature that every implementation HuxerUI
builds with offers in C++20 mode as well — the MSVC STL's named modules are
documented as needing `/std:c++20` or later.

**Everything is c++20**, the standard of the SDK's ABI and of a CMake
application: the framework, its test suites, the templates and the examples.
An application's graph also compiles the C++ standard library of the macOS and
iOS rows, `llvm.libcxx`, whose sources need c++23; from mcpp 2026.9.15.2 a
provider's `[package] standard` compiles its own sources
([mcpp#641](https://github.com/mcpp-community/mcpp/issues/641), item 2), and
`llvm.libcxx` 22.1.8.3 states it, so the application's graph stays at c++20.
The templates and the examples declare that library themselves, because a
library that declared it would reach every consumer. The earlier reason for
c++23 on Windows ([mcpp#603](https://github.com/mcpp-community/mcpp/issues/603))
is fixed too.

`[toolchain] windows = "msvc@system"` would sidestep it and put both build
systems on one compiler, and is blocked separately: a host module's BMI reaches
`cl.exe` in clang's `name=path` spelling and the compile dies with `C1083`
([mcpp#604](https://github.com/mcpp-community/mcpp/issues/604)).

### Resources are a module too

`hrc` writes the accessor header a CMake project includes
(`<namespace>_resources.h`) and, for the mcpp build, the same declarations as
a module interface unit, `<namespace>.resources` — `export module
app.resources; import huxerui; export namespace app { … }`. The rule declares
that unit to mcpp as a generated module interface (`provides`/`imports` on
the hrc edge, the mechanism hcg's transformed units use), so a module-style
application writes `import app.resources;` and no header at all; the live2d
template does. A header-style package gets the header only: the rule
forces `<typeinfo>` and the scope prelude into its translation units, and a
forced include before `export module` is what no compiler accepts.

### Macros are the exception

`HUXERUI_SCOPE` and friends are macros, and macros do not cross a module
boundary. Two different holes, two different fixes:

- **Generated code.** `hcg` injects the macro's *expansion*, not its name, so a
  transformed composable compiles under `import` and under `#include` alike.
  Preprocessing the two forms yields identical tokens, so the CMake build sees
  no change at all. See [Composable Code Generation](composable-codegen.md).
- **Hand-written code.** A scope written by hand is spelled the way the macro
  expands — `return Scope([=]() -> View { … });` — and `Scope` and `View` are
  exported, so a module unit needs neither the macro nor a header.
  `mcpp/examples/02-module-units/src/banner.cppm` is the worked case.
  `<huxerui_scope_prelude.h>` remains for a **non-module** translation unit in
  an mcpp project, which the rule force-includes; that is the only case left
  where the macro is reachable and needed.

The prelude is generated from `view.h` by the same `huxerui-module-gen`, so the
two definitions cannot drift into an illegal redefinition.

## 5. Linux dependencies come from xlings, not from the machine

mcpp is payload-first: it compiles with its own toolchain and its own glibc.
Reaching for the host's GTK mixes two C libraries in one binary, and it does
not fail politely. `pkg-config --cflags gtk4` on Debian emits the system
multiarch include directory, the payload glibc's `<time.h>` then finds the
**system's** `<bits/time.h>`, and every core translation unit fails:

```
error: 'time' has not been declared in '::'
error: 'struct timespec' has no member named 'tv_sec'
```

Not only the platform units — a build program's include directories colour the
whole package. `-idirafter` makes it compile again and is still wrong: the link
would then join libraries built against one glibc to objects compiled against
another.

So the transitive `.pc` closure of `gtk4 + epoxy + libsoup` is declared as
xlings payloads and `build.mcpp` points `PKG_CONFIG_LIBDIR` at those only,
which keeps the host out entirely. They are declared on the **target** axis
(`[target.'cfg(linux)'.xlings.workspace]`) because they are what the produced
code is compiled and linked against.

### The table is declared once, and a library calls the rule

`xpkg_dir` answers only for payloads the *building* package declared, and a
host module's `[xlings.workspace]` counts as that package's own. So the 37
entries live in `mcpp/huxerui-build-rules-gtk/mcpp.toml` — the rule package is
a host module compiled into the framework's, every application's and every
library's build program — and nowhere else; measured: with the root
manifest's copy removed, the framework builds, the example runs and its
RPATH is byte-identical. `huxerui::rules::linux_gtk(link)` runs the pkg-config
probe wherever it is called: the framework's `build.mcpp` calls it with
`link` (the `-l`/`-L` half reaches every consumer's final link), and a
library whose own sources include GTK headers calls it without, because the
`-I` half of a dependency's probe colours the dependency's translation units
only. Lib-Live2D's GL surface is that library, and its manifest declares no
GTK. Four of the entries are what the probe asks for (`gtk4`, `libepoxy`,
`libsoup`, `glib`); the other 33 are their `.pc` `Requires` closure, spelled
out because pkg-config searches the declared payloads and nothing else, and
pinned exactly because that is what makes two machines build one binary.
`huxerui-build-check` fails if the root manifest grows a second copy.

### Dialect flags belong to the whole graph

`pkg-config --cflags gtk4` also emits `-pthread -msse -msse2 -mfpmath=sse`. A
`cxxflag` emitted by a build program colours that package's own translation
units and nothing else, so the framework compiled with them and every consumer
compiled without. clang records the thread model and target-feature set in the
BMI it writes and refuses one that disagrees, which surfaces as every name in
`import huxerui;` being undefined — nineteen errors that never mention a
module. GCC's check is laxer, which is the only reason this held together until
the build ran on clang.

All of them are dropped. The `-m` flags because SSE2 is baseline on x86-64, so
naming them explicitly only put a feature list in the BMI; `-pthread` because
nothing needs it — no translation unit in the graph carries it, so every BMI
agrees, and the payload glibc (2.44) has pthread in libc, so `std::thread`
links and runs without it. It was `[build] dialect_cxxflags` until 2026-09-13,
which reached every consumer's translation units too; §7a records why the
typed replacement was not adopted either.

## 6. Platform interfaces

**Windows.** mcpp builds with clang targeting `x86_64-windows-msvc` — the MSVC
ABI and the Windows SDK, not MinGW — so MSVC-dialect flags (`/W4`,
`/permissive-`, `/utf-8`) are rejected by its GNU driver and are absent.
`[target.windows.runtime] libraries` names five libraries CMake never states
either: `CMAKE_CXX_STANDARD_LIBRARIES` adds kernel32, gdi32, winspool, uuid and
comdlg32 implicitly, and mcpp links exactly what the manifest names. Omitting
them surfaces at the link as `undefined symbol: CreateDIBSection`.

Windows application templates and examples select the GUI subsystem with `[targets.<name>] windows_subsystem = "windows"` (mcpp 2026.9.12.2+, mcpp-community/mcpp#618), which preserves CRT initialization and the portable `main()` signature without creating a console. mcpp renders `/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup` on the MSVC ABI and `-mwindows` on the GNU one, and nothing at all on ELF and Mach-O. It is a per-target field rather than a package-level `ldflags` or `link-flag` entry because those channels also reach tests and consumers; `huxerui.rules::configure()` therefore still emits no such flag, since component libraries use it too. Until that mcpp release the same selection was made with `#pragma comment(linker, ...)` in each entry source, which only ever worked on the MSVC ABI. CMake makes the equivalent selection on each application target in `huxerui_add_app`.

**macOS.** `.mm` is a first-class source kind but is not in the default glob,
so `platform/macos/*.mm` is listed explicitly. `frameworks` is a **top-level**
`[runtime]` key — the per-target vocabulary is `libraries` only, and a
`[target.macos.runtime] frameworks` entry is ignored, failing at link on macOS
alone. Unconditional is correct: the engine renders it only for Mach-O.

## 7. Distribution formats

`mcpp pack` owns the mechanism and the two universal shapes, `tar` and `dir`;
every other format lives in a package that declares it and is reached with
`mcpp pack --format <name>`. The packages here are the `dist-*` members of
`mcpp:plugins` (0.11.1 is the floor), and `huxerui.rules` is what puts them on
every application's build program: it is a host module that `huxerui`
re-exports, so an application names neither the members nor the payloads they
run, nor the runners.

| Format | Row | Member | Runs through |
|---|---|---|---|
| `setup`, `msi` | Windows | `dist-wix` (`xim:wix`) | — |
| `appimage` | Linux | `dist-appimage` (`xim:appimagetool`) | — |
| `dmg`, `app` | macOS | `dist-apple` (Xcode's toolchain) | `macapp-run`, supplied by the member |
| `app` | iOS | `dist-apple` | `simctl-run` on the simulator, supplied by the rule; `devicectl-run` on a device, supplied by the member |
| `apk` | Android | `dist-apk` (`xim:android-build-tools`, `xim:android-platform`, `xim:jdk-temurin`, `xim:android-debug-keystore`) | `adb-run`, supplied by the rule |
| `web` | Web | `dist-web` (a static directory) | any static HTTP server |

`huxerui package` under CMake and under mcpp produces the same artifact per
platform -- the Setup.exe, the AppImage, the DMG, the two-ABI APK, the `.app`,
the Web directory -- and publishes it to `dist/<platform>/`: under mcpp the CLI
reads the paths `mcpp pack` reports on its `Packed` lines.

**Provided on the row, with nothing stated.** The rule calls
`mcpp::provides_pack_format(...)` for the format of the row it is building,
on every build, so `mcpp pack --format apk` on a fresh `huxerui create --build
mcpp` project produces the APK Gradle's release variant does and `mcpp pack
--format appimage` the AppImage CMake's package does; `mcpp pack --format apk`
on a Linux desktop build is an *unknown* format, which is the true answer. A member's
plan is a no-op until `mcpp::pack_format()` names its format, so a plain
`mcpp build` produces no installer and no bundle. The `configure()` options
only change what a format produces:

```cpp
huxerui::rules::configure({
    .target            = "myapp",
    .resources         = "resources",
    .bundle_identifier = "org.example.myapp",
    .installer         = { .upgrade_code = "27B7A054-FFE4-48A5-92E3-5D90C0507EEB" },
    .appimage          = { .icon = "assets/app.png", .categories = { "Graphics" } },
    .apple             = { .icon = "assets/AppIcon" },
    .web               = { .title = "My App" },
});
```

Designators follow the declaration order, which GCC enforces.
`bundle_identifier` and `bundle_name` are `huxerui_add_app()`'s
`BUNDLE_IDENTIFIER` and `BUNDLE_NAME`: one id for the `.app`, the APK and the
Windows package (its manufacturer and both upgrade codes, derived as CMake
derives them), which `.apple.bundle_id`, `.android.application_id` or
`.installer.project_id` still override.
Every field has a default the member derives from `[package]` — the version,
the bundle identifier and the application id from the namespace and name, the
upgrade code deterministically from the package identity, the manufacturer
from the authors. `target` is the one value worth stating: mcpp tells a build
program the package name and not its targets, and the desktop `.resources`
directory (§3) is named after the target.

**What the rule adds to each member** is what the CMake templates give a
project, so a default package is the specification's program model:

- **Web.** The page is the rule's template
  (`mcpp/huxerui-build-rules-dist/web/index.html.in`), which imports the
  MODULARIZE launcher the emscripten section exports and mounts the application;
  an application replaces it with `.web.template_file`. As in CMake's Web
  template, the storage key that scopes the application's browser-managed files
  is its bundle identifier, and the SDK's favicon and touch icon are deployed
  beside the launcher as `<target>.favicon.svg` and
  `<target>.apple-touch-icon.png`.
- **AppImage.** The SDK's Linux icon (an SVG, kept as one), and the file named
  `<target>-<version>.AppImage`.
- **Apple bundles.** The Info.plist carries the entries CMake's templates write
  (the display name; on iOS a launch screen, the development region, the
  dictionary version and the supported orientations), with the application's
  own entries -- `ios/Info.plist` or `macos/Info.plist`, or `.apple.info_plist`
  -- merged over them; a key the bundle derives is refused. The icon is the
  SDK's (`AppIcon.icns`, the iOS asset catalog's PNGs) unless `.apple.icon`
  names one. `.apple.identity`, `.entitlements` and `.provisioning_profile`
  sign a bundle; the device row runs through `devicectl-run` and `huxerui run
  ios --device` reaches it, which no device in CI has measured. The resources
  are deployed into `HuxerUI/` by §3, and dist-apple places every deployed
  directory at the bundle's resource destination -- `Contents/Resources/HuxerUI`
  on macOS, the flat root on iOS -- where `huxerui_add_resources` stages it for
  a CMake bundle and where the adapters read, with no fallback. A bundle is what
  runs: `mcpp run --format app` hands it to the row's runner, and `--format dmg`
  puts it beside an `Applications` link in a disk image.
- **APK.** dist-apk's level 1. The framework's Java host
  (`platform/android/huxerui/src/main/java`) is the first source root, the
  application's `android/java` and `android/kotlin` the next (Kotlin needs the
  framework's `android-kotlin` feature, which reaches `dist-apk-kotlin`), and
  `android/res` its resources when it has one, else the launcher icon set of the
  SDK's Gradle template, so the icon lives once in the repository. The rule
  writes the two classes Gradle generates for the template:
  `<application id>.MainActivity`, a subclass of `HuxerUIActivity` whose static
  initialiser loads the application's shared object, and `BuildConfig` with the
  variant's fields and `HUXERUI_APP_LIBRARY`. The manifest is the Gradle
  template's as the Android Gradle plugin packages it: no permission, the same
  `<application>` and `<activity>` attributes, `android:extractNativeLibs="false"`,
  versions from `[package] version`. dist-apk strips the native libraries and,
  because the manifest asks, stores them uncompressed on a 16 KB page. The
  framework is its own `libhuxerui.so` beside `lib<app>.so`, loaded by name from
  `HuxerUIView` as the Gradle build does (§1), for every `--target` row in one
  APK. A release package is unsigned and a `dev` one signed with the debug key,
  as Gradle's variants are; `.android.keystore` signs either. A direct HuxerUI
  library contributes its `android/java`, `android/kotlin`, `android/res`,
  `android/assets`, `android/libs` and `android/AndroidManifest.xml` (§3), an
  application's `android/libs/*.jar` and `*.aar` join the package, and
  `.android.maven` coordinates, with the framework's `android-maven` feature,
  are resolved into `android/maven.lock` by `MCPP_DIST_APK_MAVEN=update` and
  read from it by every other pack.

**The Windows Setup.exe is CMake's.** `huxerui package windows` produces
`<target>-Setup-<version>.exe`: a Burn bundle chaining the application's MSI,
whose interface is a second HuxerUI program, `<target>-Installer.exe` --
`platform/windows/windows_installer.cpp` with the project's page and ten
locales. Under mcpp that program is the application's `windows/installer`
package (`configure({ .bootstrapper = true })`), a tool of the application's
Windows rows declared behind its `windows-installer` feature
(`[target.'cfg(os = "windows")'.feature-deps.windows-installer]`). It is built
for a package build only, as under CMake: `huxerui package windows` runs `mcpp
pack --format setup --features windows-installer`, which reaches every pass of
the pack ([mcpp#641](https://github.com/mcpp-community/mcpp/issues/641), item
4), and the rule refuses a Setup.exe whose build did not name the feature. The
package carries only its entry, and the rule gives it
the page and strings of the SDK's Windows template until the package carries
its own. A tool publishes its binary and nothing
beside it, so the application's rule compiles the interface's resource package
itself and hands it, with WiX's `mbanative.dll`, to the bundle as payloads. The
two WiX definitions are rendered from the templates CMake's Windows package
renders (`tools/huxerui_cli/templates/platform/windows/app/package/*.wxs.in`),
differing only in how a file reaches WiX: dist-wix names the program and every
staged file rather than harvesting a bind path. The glue is compiled through a
one-line unit in the build's output directory because the tool store's own
prefix leaves a source under the SDK's root past Windows' 260 characters.

**When a member declines, the build says why.** A member's plan reports its
reason on stderr, which mcpp discards when the build program succeeds — so
`mcpp pack --format apk` used to fail as `no action claimed --format 'apk'`
with the reason gone. The rule repeats it through `mcpp::warning` whenever the
declined format is the one `mcpp pack` asked for:
`huxerui.rules: dist-apk declined this build: unknown manifest template token`.

**The API level the Java host compiles against.** The framework's Java uses
API 36 (`Build.VERSION_CODES.BAKLAVA`, the accessibility `CHECKED_STATE_*`
set), the `compileSdk` its Gradle build states; dist-apk pins
`xim:android-platform` 36-r2 on its own feature axis since 0.9.1, so the rule
declares nothing.

## 7a. The artifact's ABI switches: exceptions on the Web, threads nowhere

Two properties of an artifact cannot differ between its translation units:
clang records the thread model and the exception model in the BMI it writes
and refuses an importer that disagrees, which surfaces as every name in
`import huxerui;` being undefined. mcpp's typed form is `[target.<selector>.abi]
threads / exceptions` on the **root** manifest, applied to the standard library
prebuild, the scan, every translation unit and the link; a dependency states
what it needs with `requires_abi`, on the target axis since 2026.9.12.3, and a
root that does not satisfy it is refused before anything compiles.

**Exceptions.** Emscripten's default is off and HuxerUI validates arguments
with exceptions, so `mcpp.toml` carries `[target.'cfg(os = "emscripten")']
requires_abi = { exceptions = true }` and every application manifest the
templates write carries the matching `abi` table. A root that lacks it is
refused naming the member and the selector; without the refusal the failure is
at run time, `Aborted(Assertion failed: Exception thrown, but exception
catching is not enabled)`, and names neither.

**Threads.** Not required and not set, and the reason is what §5 measured: no
translation unit carries `-pthread` any more, so the BMIs agree, and the payload
glibc has pthread in libc. Requiring the switch would also have put an `abi`
table on this package for its own root build, which the engine reports in every
consumer's build as a table only the root decides; a requirement nothing needs
is not worth a warning everything sees.

An older engine (2026.9.12.2 and before) ignores a target-axis `requires_abi`
silently, which is why mcpp/README.md states a floor rather
than relying on the refusal to reach an old client.

## 8. Verification

| Check | What it catches |
|---|---|
| `huxerui-build-check` | a source, define or platform library in one build system and not the other |
| `huxerui-module-gen --check` | a public name added to a header and not to the module shell |
| `mcpp test` in `huxerui-source-select` | the scanning the rules and the tools share |
| `mcpp test` in `huxerui-tests/{unit,runtime,ui,smoke}` | CMake's four suites against the mcpp-built framework, with profiling on as CMake builds its own tree |
| `mcpp build --workspace` | a workspace member that does not build |
| `mcpp pack` shape | headers, module interface and library all present |
| `build-systems-conformance.yml` | a default package whose program model differs from CMake's beyond the specification's allowed differences |

Both tools are C++ programs built by mcpp: a repository that offers mcpp as a
first-class build system should not need a second language to check its own
build.

CI covers Linux with GCC, Linux with clang, macOS and Windows. The clang leg is
not redundant with the other two — Windows and macOS compile a BMI and its
consumer in the same job with the same flags, so a dialect divergence like §5's
shows only where GCC is the default and clang is the exception.

## 9. Known gaps

**The extension points a developer has under CMake** are the specification's
§3, with each one mcpp does not yet support named there and tracked:
- a library's Swift sources (mcpp-community/mcpp#647, E2);
- the resources and Android sources of a library the application reaches only
  through another library (E1);
- an Android application built on a macOS or Windows host (E3, and the NDK's
  Windows archive).

An iOS device build is supported and not measured, because no CI runner has a
device.

**A static framework on Android is not an option yet.** The defaults follow
CMake (§1). A shared framework on the desktop is the application's
`linkage = "shared"`, and the `.app` carries it in `Contents/Frameworks/`. On
macOS that option builds with the toolchain's libc++ and its 14.0 floor: a C++
shared library in a graph whose C++ runtime is a package is refused
(mcpp-community/mcpp#641, item 5), so it cannot take the application's
`llvm.libcxx`, and whether a standard exception thrown in the framework is
caught by type in the application is mcpp-community/mcpp#646, F2. The opposite
departure on Android is not one line: `HuxerUIView`'s static initialiser loads
`libhuxerui.so` by name, and keeping that failure behaviour means a static
build must choose its artifact and its loading entry together, as one explicit
configuration. mcpp 2026.9.15.2 lets an application state a per-row linkage and
a build program read it (`mcpp::dep_linkage`); the Java host's side of that
configuration is a follow-up.

**The smoke is windowless, as CMake's is.** `tests/platform/testing/smoke.cpp`
runs under `mcpp test` in `mcpp/huxerui-tests` on the desktop rows (§8), on
virtual frames. No CI row starts a real window; the Android and iOS rows
install and launch, and stop there.

**`tests/cli` is not in the mcpp suites.** It links the CLI, a CMake target.
The unit, runtime, UI testing and smoke suites are (§8); the fixtures CMake
generates for them are compiled by each suite's build program.
