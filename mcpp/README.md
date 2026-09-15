# Developing HuxerUI and its applications with mcpp

HuxerUI has two build systems. CMake is the one the release SDK, the Gradle and
Xcode shells and store signing go through; mcpp builds the framework and
applications on the same six platforms -- Linux, Windows, macOS, iOS, Android and
the Web. By default the two build the same program: how the framework is linked,
how an application is loaded, the platform floors, and what `huxerui package`
produces. Neither reads the other's files, and deleting this directory plus
`mcpp.toml` and `build.mcpp` returns the repository to a CMake-only project.

The path for users -- environment, commands, formats and what mcpp adds -- is
[C++20/23 Modules and mcpp: Six Platforms](../docs/guide/cpp-modules-and-mcpp.md);
the design and the open gaps are in
[`docs/design/mcpp-build-system.md`](../docs/design/mcpp-build-system.md);
three worked applications are in [`examples/`](examples/).

## Building

```bash
mcpp build                          # the framework
cd mcpp/examples/01-import && mcpp build  # an application on top of it
```

**Nothing needs to be installed first.** The GTK 4 stack comes from the xlings
payloads declared in `../mcpp.toml`, which mcpp provisions on the first build.

## What lives here

| Path | Contents |
|---|---|
| `huxerui-build-rules/` | `huxerui.rules`, the module a consumer's `build.mcpp` imports |
| `huxerui-source-select/` | the pure half of the rules -- globbing, the composable pre-filter, header scanning, the Windows installer definitions -- split out so `mcpp test` can reach it |
| `huxerui-tools/` | `huxerui-build-check` (parity) and `huxerui-module-gen` (the module shell and scope prelude) |
| `huxerui-tests/` | the unit, runtime, UI testing and smoke suites, one package each, as CMake builds one executable per suite |

Elsewhere: `../modules/huxerui.cppm` is the C++20 module front door (generated
by `huxerui-module-gen`), and `../tools/{codegen,resource_compiler}/mcpp.toml`
make `hcg` and `hrc` buildable packages.

Both tools were Python once. They are C++ now for the reason this directory
exists: a repository that offers mcpp as a first-class build system should not
need a second language to check its own build. The rewrite was not free of
consequence -- the C++ header scanner finds five public names the Python one
missed, all of which compile, and correctly drops one it exported that was a
template parameter rather than a type.

`../mcpp.toml` is the framework package and the workspace root; `../build.mcpp`
resolves the Linux platform dependencies and produces the builtin resource
header.

## Writing an application

The manifest names one dependency and the build program makes one call:

```toml
[dependencies]
huxerui.huxerui = { path = "../.." }        # or a version, once published
```

```cpp
import mcpp;
import huxerui.rules;
int main() { return huxerui::rules::configure({ .resources = "resources" }) ? 0 : 1; }
```

`configure()` schedules the composable transform (`hcg`) and the resource
compiler (`hrc`) as build-graph edges. GTK, the HuxerUI library and the include
path arrive through the dependency edge: a dependency's `build.mcpp` emits
`link-lib` / `link-search` that reach the **final** link, so an application
never restates them. `mcpp/examples/` holds three worked examples.

On Windows the application templates and examples keep a portable `int main()` and declare `windows_subsystem = "windows"` on their `[targets.*]` executable (mcpp 2026.9.12.2+). Double-clicking the resulting executable does not create a console; standard output and standard error have no automatically created console. The key reaches that executable's link alone, which is why it is not `[build] ldflags` -- that channel also reaches the test binaries and consumers. See [Platform interfaces](../docs/design/mcpp-build-system.md#6-platform-interfaces) for the build-system boundary.

## Three things worth knowing before editing

**The GTK stack is NOT the machine's.** mcpp compiles with its own toolchain and
its own glibc, so reaching for the host's GTK mixes two C libraries in one
binary. Measured: `pkg-config --cflags gtk4` emits
`-I/usr/include/x86_64-linux-gnu`, the payload glibc's `<time.h>` then reaches
the system's `<bits/time.h>`, and every core translation unit fails on
`'time' has not been declared in '::'`. The payload list in `../mcpp.toml` is
the transitive `.pc` closure, pinned. CMake keeps using the distribution's
packages, as `cmake/platform/Linux.cmake` and AGENTS.md require.

**The host tools are built from source, not taken from `tools/prebuilt/`.**
Those binaries are produced for the host's C library by
`.github/workflows/update-host-tools.yml`, and after a change to
`tools/codegen/transform.cpp` they are stale until that workflow runs on main.
CMake keeps using them; the mcpp leg builds its own with mcpp's toolchain.

**The builtin resources are compiled twice.** The framework compiles them for
the header its own 11 translation units `#include`; an application compiles
them again for the package it merges with its own resources. mcpp has no
channel from a dependency's build output to a consumer's build input -- a
consumer is given `dep_dir()`, the dependency's *source* root -- and inventing
one would mean writing into a package root that may be read-only. 44 files /
196 KB, incrementally cached; the alternatives are enumerated in the plan.

## Six platforms, one manifest

A module-style application names its resources with `import app.resources;`: hrc
writes that module beside the header, and the rule declares it to mcpp as a
generated module interface (design §4). The Linux GTK payload table lives once,
in `mcpp/huxerui-build-rules-gtk/mcpp.toml` (design §5).

`huxerui create app <name> --build mcpp --template live2d` is the same story for an
ecosystem library: a Live2D model on all six rows through one
`huxerui.live2d` dependency, nothing about Cubism in the application.

The framework's `mcpp.toml` carries one section per target row, and an
application builds for any of them by naming the row:

```bash
mcpp build --target x86_64-linux-gnu            # or aarch64-macos, x86_64-windows-msvc
mcpp build --target wasm32-emscripten
mcpp build --target aarch64-ios-sim
mcpp build --target x86_64-linux-android
```

NDK, emsdk, JDK, simulator and emulator tooling are xlings payloads mcpp
installs on first use; iOS additionally needs Xcode on the machine, because
Apple's SDK is located rather than installed. **Floor: mcpp 2026.9.15.2 and
`mcpp:plugins` 0.11.1** — the framework states its Android form per row
(`[target.<row>.targets.huxerui] kind = "shared"`) and its platform floors as
`version-floor` requirements, the test suites are packages with `[test]
discover`, the distribution members read the closure the engine stages and
take the platform code, metadata and signing of the
[specification's extension points](../docs/design/build-systems-spec.md#3-developer-extension-points),
and `mcpp pack --features` builds the Windows installer interface for a package
only; none of that exists below those releases. On macOS and iOS an application
builds its own C++ standard library (`llvm.libcxx` 22.1.8.3), which states the
c++23 its sources need, so the application stays at c++20
(mcpp-community/mcpp#641).

## Distribution formats

`mcpp pack --format <name>` produces what a user installs, and `mcpp run
--target <row> --format <name>` packages and runs it where it runs.
`huxerui package` runs the format CMake's package build corresponds to --
`setup`, `appimage`, `dmg`, `apk`, `app` and `web`; the guide lists the
[rows and runners](../docs/guide/cpp-modules-and-mcpp.md#distribution) and the
formats mcpp adds, such as `msi` and `aab`, among its
[enhancements](../docs/guide/cpp-modules-and-mcpp.md#build-and-development-enhancements).

Every format is a member of `mcpp:plugins` reached through `huxerui.rules`,
which provides each one on the row it serves without being asked: a fresh
project packs every format with nothing but `.target` in its build program,
and `configure({ .bundle_identifier, .installer, .appimage, .apple, .web, .android })` only
changes what a format produces. The members declare the payloads they run and
the rules supply the runners, so an application declares neither.
See [Distribution formats](../docs/design/mcpp-build-system.md#7-distribution-formats).

**The Windows Setup.exe's interface is the application's.** An application
carries it as `windows/installer`, a package its Windows rows build as a host
tool, so `mcpp pack --format setup` -- what `huxerui package windows` runs --
produces the bundle CMake's package build produces. CMake builds the installer
only for a package build; mcpp builds a tool once per source and toolchain for
every build that declares it, because `mcpp pack` takes no feature a narrower
declaration could hang on, so the first Windows build pays for it once. The WiX
definitions are rendered from the same templates CMake's Windows package
renders.

The framework is linked the way CMake links it: statically into the
application everywhere but Android, where it is its own `libhuxerui.so` loaded
by name from the Java host, as the Gradle build sets `HUXERUI_BUILD_SHARED=ON`.
The framework's manifest states that form per row, so an application writes one
dependency line, and dist-apk carries the library into the APK.
`mcpp pack --target aarch64-linux-android --target x86_64-linux-android --format
apk` builds the two-ABI APK CMake's Gradle template builds.

**Compile-time profiling is a feature, `profiling`, off by default**, as the
SDK a CMake application builds against carries none. The repository's own
examples and test suites turn it on, as CMake builds its own tree.

The suites are CMake's: `mcpp test` in `huxerui-tests/unit`, `runtime`, `ui`
and `smoke` builds and runs each against the mcpp-built framework, the smoke
being the windowless UI testing smoke (`tests/platform/testing/smoke.cpp`); no
row starts a display in CI.

## Packaging

```bash
mcpp pack
```

writes `target/dist/huxerui-<version>-<compatibility tag>.tar.gz` containing
`include/huxerui/**`, `interface/huxerui.cppm` and `lib/<triple>/libhuxerui.a`
-- both consumption paths and the binary, with no packaging script of its own.
It is per-leg by construction: a package for another platform comes from that
platform's runner, and `mcpp pack` says so when `[package] platforms` claims one
that was not packed.

**This is not the release SDK.** That one ships a CMake package config for
CMake consumers and is produced by `cmake/HuxerUISdk.cmake` and
`scripts/package_sdk.sh`; `mcpp pack` at the root produces the library for
mcpp consumers, one leg per runner.

## Parity

Two build systems maintaining one set of facts drift, and the drift is quiet:
add `platform/windows/win32_foo.cpp`, forget `mcpp.toml`, and the Windows mcpp
build fails at link with `undefined reference`, far from the cause.

```bash
cd mcpp/huxerui-tools && mcpp run huxerui-build-check
```

is the first job in `.github/workflows/mcpp-build.yml`. It already caught one
real defect: a bare
`platform/windows/*.cpp` glob swept in `windows_installer.cpp`, which is the
Burn glue each application's installer interface compiles
(`cmake/HuxerUIWindowsInstaller.cmake`), not part of the framework.
