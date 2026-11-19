# C++ Modules and mcpp

Use this reference for a C++ modules project: its sources are module units that write `import huxerui;`, its `mcpp.toml` depends on `huxerui`, and its `build.mcpp` imports `huxerui.rules`. `import huxerui;` exposes the same public entities as the umbrella header. Preserve the dependency version and build setup selected by the project.

A headers project that also carries an mcpp declaration (`--build-system cmake,mcpp`) builds with the same rules; its sources stay as described in the other references. Such a library, which is a library's default and the recommended form, is also the module named by its C++ namespace: the mcpp build generates `modules/<module>.cppm` from its public headers, so do not edit that file; to keep a hand-written one, set `cpp_modules_gen = false` under `[package.metadata.huxerui]` and export every public name it reports. An existing headers library gains that interface as [Add C++ modules to an existing headers library](#add-c-modules-to-an-existing-headers-library) describes.

Drive the project with the HuxerUI CLI, which dispatches to mcpp:

```bash
huxerui build linux
huxerui run linux
huxerui package linux
```

The mcpp integration builds Linux, Windows, macOS, the Web, Android, and iOS. `huxerui package <platform>` writes the platform's package through the official `mcpp:plugins` members, and Android, iOS, and macOS packages need the `id` in `[package.metadata.huxerui]`. `mcpp build`, `mcpp run`, `mcpp test`, and `mcpp pack --pkg-format <format>` remain available underneath.

For an existing project that manually links prebuilt SDK binaries instead of using the package rules, read [mcpp-build.md](mcpp-build.md).

## Project structure

```text
mcpp.toml               package, dependency, targets, and language standard
build.mcpp              HuxerUI build-rule configuration
src/app.cppm            application module: the page, its composables, and the Application object
platform/<id>/main.cpp  each desktop platform's process entry; the Android, iOS, and Web shells as in any project
resources/              optional resource input
```

Create a C++ modules application with `huxerui create app <name> --build-system mcpp`; `huxerui create library <name> --build-system mcpp` creates a C++ modules library. mcpp alone selects C++ modules, and a build-system list that names `cmake` selects headers. `--template` selects the application content (`app` or `navigation`), and `--platform` selects the platform shells as for any other project. Inspect the generated manifest rather than copying a fixed dependency version into an existing project.

A C++ modules library exports a module named after its C++ namespace, records its identity in `[package.metadata.huxerui]`, and carries an `examples/preview` application that imports it; `huxerui run <platform>` from the library tree runs that Preview. Its Android platform package under `platform/android` reaches an application's package without further declaration; state `[package.metadata.dist-apk]` only to replace that inference, for example to add Maven coordinates.

The build program imports the HuxerUI rules:

```cpp
import mcpp;
import huxerui.rules;

int main() {
  return huxerui::rules::configure({.resources = "resources"}) ? 0 : 1;
}
```

Keep `[build] sources = []` when the rules own source discovery. They transform composables and add the resulting sources; also globbing the originals can link duplicate definitions. Preserve the generated application target and each platform's `main` path.

The rules compile and stage resource packages through `hrc`. An application's resource namespace is `app`, as in a CMake application, unless `resource_namespace` is supplied; a library's follows its public identity. A package written as module units reaches its resources with `import <namespace>.resources;`, such as `import app.resources;`, and needs no header.

## Module code

```cpp
export module app;

import std;
import huxerui;

using namespace huxerui;

[[huxerui::composable]]
View Counter() {
  auto count = UseState(0);
  return Row {
    Button("Count").OnClick([count] { count += 1; }),
    Text(count),
  };
}
```

Use module imports rather than `#include` in the generated module application. Import `std` in units that instantiate HuxerUI templates or use standard-library entities; importing HuxerUI does not make all standard-library declarations reachable to the caller.

mcpp projects use C++23, mcpp's default: `huxerui.rules` runs in the build program and imports `mcpp:plugins` members that need it. The CMake path keeps its C++20 floor. A newer language mode alone does not solve binary compatibility with a prebuilt SDK.

Define annotated composables in source or module units processed by `huxerui.rules`. The application root already owns a scope. Keep each `platform/<id>/main.cpp` focused on required entry setup and `RunApplication()`; platform registration required before startup can still live there.

Macros do not cross module boundaries. Prefer `[[huxerui::composable]]`; when explicitly writing a scope, use the exported `Scope` and `View` APIs rather than assuming `HUXERUI_SCOPE` is imported.

## Add C++ modules to an existing headers library

A headers library becomes importable as a C++ module without changing its sources or its CMake declaration: the mcpp build generates the module interface from its public headers.
It needs its public headers below `include/` and its public names in one C++ namespace.
The module is named after that namespace with `::` written as `.`, such as `acme.widgets` for `acme::widgets`.

1. Without an mcpp declaration, take `build.mcpp` and `mcpp.toml` from a library `huxerui create library <name> --build-system cmake,mcpp --target <public-cmake-target> --namespace <cpp-namespace>` writes with the same SDK, so the dependency on HuxerUI matches it, and add each library dependency to `[dependencies]` as a path or package entry.
2. In `mcpp.toml`, name the namespace and the interface:

   ```toml
   [package.metadata.huxerui]
   namespace = "acme::widgets"

   [lib]
   path = "modules/acme.widgets.cppm"
   ```

3. Create `modules/acme.widgets.cppm` as an empty file; mcpp requires the `[lib]` path to exist before the build writes it.
4. Build in the library root with `mcpp build`. The first build reports `generated modules/acme.widgets.cppm from the public headers (<n> names)`. Commit the file and do not edit it: later builds rewrite it whenever the headers change.
5. Resolve the build's warnings. A macro a header defines does not reach `import`, so a translation unit that uses it also includes that header.

The interface exports the names declared directly in the namespace: functions, classes, templates, enumerations, aliases, and variables.
Names in a nested namespace, such as `detail`, and names declared `static` are not exported; move a name a consumer needs into the namespace itself.
A consumer built with mcpp depends on the library and writes `import acme.widgets;`, and a consumer built with CMake keeps including the headers; both reach the same entities, so the library needs no second binary.
To write the interface by hand instead, set `cpp_modules_gen = false` under `[package.metadata.huxerui]`; the build then reports each public name the file does not export.

## Build and verification

```bash
huxerui build <platform>
huxerui run <platform>
mcpp test
```

Reuse the project's selected toolchain. Do not install a fixed compiler version or change the user's default toolchain merely because this reference includes mcpp. Inspect the installed command help and package configuration when selecting targets, workspace builds, tests, or packaging.

Use the resolved package's public interface and user documentation for HuxerUI APIs. A source checkout is not required beside an application, and relative paths into the framework repository are not portable skill references.

For manifest or build-program syntax, consult the documentation matching the installed mcpp version:

- [mcpp documentation](https://github.com/mcpp-community/mcpp/tree/main/docs)
- [Manifest](https://github.com/mcpp-community/mcpp/blob/main/docs/04-mcpp-toml.md)
- [Dependencies](https://github.com/mcpp-community/mcpp/blob/main/docs/05-dependencies.md)
- [Testing](https://github.com/mcpp-community/mcpp/blob/main/docs/08-testing.md)
- [Build programs](https://github.com/mcpp-community/mcpp/blob/main/docs/30-build-mcpp.md)

The default-branch links are discovery entry points, not proof that an older installed version accepts the same syntax.
