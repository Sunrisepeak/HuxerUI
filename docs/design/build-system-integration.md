# Build System Integration Contract

> Status: Draft for review. This proposes the standard build model for HuxerUI projects and the contract every build backend implements; it does not claim that every current workflow implements it.

HuxerUI projects are built by a build backend selected per project.
CMake is the first implementation of this contract and mcpp is the second; xmake and other build systems may implement it through their own project formats, dependency resolution, and target APIs.
No backend is the reference: the contract is stated independently of every implementation, and each backend conforms to it for its declared host/target combinations and capabilities.
The current implementations remain documented in [SDK, CLI, Platform Shell, and Library](sdk-cli.md) and [mcpp Build System](mcpp-build-system.md).

## Principles

- The contract is minimal. It specifies only what crosses a boundary between independently owned parts: the CLI and the backend, the backend and the platform shell, an application and its libraries, the build and the Runtime, and the product and its user.
- Everything that crosses no such boundary is backend-owned: compiler flags, source organization, internal targets, cache layout, toolchain selection, and dependency acquisition.
- A default build conforms to the standard model under every backend.
- A non-standard result exists only because the developer stated it explicitly, in native project configuration or on a command line, and the build reports it.
- Each project fact has one editable owner. Native build configuration and source-controlled platform shells are the sources of truth; HuxerUI adds no project manifest of its own.
- Backends agree through generated records with a shared schema, not through a shared input file.
- Common development work goes through the CLI and the platform shells without build-system-specific knowledge; detailed or non-standard configuration is done in the backend's native files and may use the backend's own commands directly.

## Terms

- **Backend**: a build-system integration that implements this contract, such as CMake or mcpp.
- **Standard model**: the project shape, default settings, application core, library form, resource behavior, and artifacts defined by this document.
- **Default build**: a project produced by `huxerui create` and built, run, or packaged with no option beyond the platform and destination.
- **Capability**: optional behavior a backend declares it supports, used only on explicit request.
- **Deviation**: an explicit developer choice that departs from a standard default.
- **Generated record**: versioned machine-readable build output that projects native configuration or build results; it is never edited.

## Responsibilities

| Owner | Responsibility |
| --- | --- |
| HuxerUI CLI | Select the project, backend, platform, framework distribution, and device; pass explicit options; orchestrate build, launch, and package publication. |
| Backend | Resolve native project settings, dependencies, and tools; generate code and resources; compile and link the application core; report effective settings and actual artifacts. |
| Platform shell and tools | Own application lifecycle, platform metadata, permissions, signing, installation, launch, and distribution containers. |

Direct native builds and CLI-driven builds must use the same target, code-generation, resource, and dependency rules.
Any shared request or result records are generated metadata, not another user-maintained project manifest.

## Project model

A project has one backend, identified by its root native build file.
The common C++ sources, the `resources/` root, and the source-controlled `platform/<platform-id>` shells have the same layout and meaning under every backend, so changing the backend replaces the root build declaration and nothing else.
A root that declares more than one backend requires an explicit selection, and the CLI diagnoses the ambiguity instead of choosing.

`huxerui create` selects the backend from the requested project style unless one is named explicitly: a header-based project defaults to CMake and a C++ modules project defaults to mcpp.
This is a creation default, not a coupling between language style and backend.
`import huxerui;` and `#include <huxerui/huxerui.h>` name the same entities under every backend that offers them.

The following work requires no build-system-specific knowledge:

- Creating a project and adding platforms.
- Building, running, packaging, and testing through the CLI.
- Editing common sources and resources.
- Editing identity, permissions, icons, signing, and other platform metadata in the platform shells.

Declaring a library dependency is one statement in the native build file.
The CLI may delegate that edit to a backend's own dependency command where one exists; it never rewrites a native build script as text.

## CLI and backend

### Build intent

`build` produces application artifacts; `run` builds the matching configuration and launches it on the selected destination; `package` builds and publishes the platform's distributable output.
The same verbs apply to every backend, and the CLI dispatches them by the project's backend rather than exposing backend-named commands.
The default logical profile is `debug` for `build` and `run`, and `release` for `package`.
The CLI reports exit code `0` for success, `2` for invalid command usage, and `1` for an operation failure, preserving the failing step and underlying tool diagnostics.

The shared request carries the following information where applicable:

| Information | Meaning |
| --- | --- |
| Operation and project | Build, run, or package; project root, application identity, and selected backend. |
| Host and target | Host OS and architecture for executable tools; target platform, optional canonical ABI list, platform SDK, and deployment minimum for produced binaries. |
| Framework selection | Selected HuxerUI SDK, package, or source checkout, including its resolved version. |
| Settings | Logical profile and explicit capability choices. |
| Destination and output | Selected device or simulator, incremental build location, and requested publication location. |

Backends map this intent to native commands and settings; compiler flags are implementation details of that mapping.
Native configuration names may differ from logical profiles, as with an Android Release variant using a CMake `RelWithDebInfo` configuration.
The request's transport and command-line spelling must be agreed when wiring a backend into the CLI; this draft specifies their meaning.

The CLI's `--abi` option selects one or more ABIs for a single target platform. The CLI resolves aliases to that platform's canonical spellings and passes a deduplicated list to the backend. An absent list preserves native project defaults. Backends must reject unsupported explicit selections and keep incremental outputs for different ABI selections distinct where the native tools require it.

Report which ABIs each produced artifact contains. Backends may produce one artifact with several ABIs where the platform supports it, or separate artifacts with unambiguous ABI-specific locations. The requested selection must be satisfied in full.

### Precedence

Explicit CLI values take precedence over matching project options, followed by standard defaults.
An omitted value preserves project configuration; explicit `off` and explicit `default` remain distinguishable from omission.
An option that does not apply to the project's backend is refused with the reason and never ignored.
A framework version pinned by native project configuration or its lock file is project policy; a CLI framework selection that the backend cannot honor without rewriting that pin is refused.
These settings govern backend-managed source targets and build steps; independent project toolchain customization remains project-owned.

### Standard settings

Every backend implements these with the stated defaults:

| Setting | Values and default | Required meaning |
| --- | --- | --- |
| Profile | `debug`, `release`; operation-dependent default above | Select development or optimized application configuration independently of capabilities. |
| Framework linkage | `static` for applications on every platform | Link the HuxerUI framework into the application, independently of system and third-party dependency linkage. |

### Capabilities

Capabilities have one shared vocabulary so a request means the same thing under every backend that supports it.
A backend declares which capabilities it supports for each host and target, and diagnoses an unsupported explicit choice rather than silently substituting another mode.

| Capability | Values | Required meaning |
| --- | --- | --- |
| Optimization | `default`, `size`, `minsize` | Keep the native profile's optimization level, prefer code size, or select a stronger size mode supported by the toolchain. |
| LTO | `on`, `off` | Enable or disable backend-managed link-time optimization in release configurations. |
| Shared framework linkage | `shared` | Link the HuxerUI framework as a shared library where the platform and backend support it. |
| C++ modules | Offered or not | Consume the framework through `import huxerui;` in addition to the public headers. |
| Additional package formats | Backend-declared names | Produce a distribution container beyond the platform's standard one. |
| Direct packaging | Backend-declared | Produce a platform package without the standard platform shell. |

A default build enables no capability.
Optimization and LTO choices apply to release configurations; they do not enable release optimization in Debug.
Backends report effective settings and their scope: prebuilt libraries retain their published settings, while source targets can be rebuilt with overrides.
Size optimization preserves the framework's required exception, RTTI, threading, ownership, and lifecycle behavior.
A capability specific to one backend is used through that backend's native configuration or commands; the CLI does not wrap it.

The current CMake mappings are `HUXERUI_OPTIMIZATION` and `HUXERUI_ENABLE_LTO`.
`HUXERUI_BUILD_SHARED` and `HUXERUI_BUILD_STATIC` select available source framework artifacts; the consuming target selects its link form.
The CLI currently exposes optimization and LTO overrides; uniform framework-linkage control is proposed by this draft.

## Backend and platform shell

Platform shells are part of the standard model.
A shell is created with the project, lives in version control, and owns the same platform facts under every backend, so platform metadata, signing, and platform extension points are configured once and do not depend on the backend.

The backend's output toward the shell is the application core:

| Platform kind | Application core |
| --- | --- |
| Platforms whose shell is a platform build project, currently Android and iOS | The application's native library or archive for each requested ABI or destination, the final resource package, and the application-integration record naming them. |
| Other platforms, currently desktop and Web | The final application binary or modules built from the common sources and the shell's source inputs, with the final resource package staged for its runtime location. |

A shell that is a platform build project invokes the project's backend to produce the application core and consumes only the generated records and the artifacts they name.
The shell does not assume a filename, and the backend does not read or duplicate shell configuration such as application identifiers, SDK levels, permissions, or signing.
The shell's backend invocation is the only part of a shell that differs between backends.

Entry points discovered by the platform must survive archive extraction, dead stripping, and LTO.
On Android, static framework integration retains the required named JNI entries and loads the application's actual shared-library name before HuxerUI Java services are used.
Libraries retain ownership of their initialization; linking archives does not combine multiple `JNI_OnLoad` implementations.

Packaging a platform application without its standard shell is a capability.
It is selected explicitly, and its result is not a default build.

## Applications and libraries

A HuxerUI library is a backend-native package with a standard form, so one library source tree serves every backend it declares:

- Public headers under `include/`, implementation under `src/`, optional resources under `resources/`, and optional platform packages under `platform/<platform-id>`.
- One public identity. The implementation name, resource namespace, and primary header derive from it through the deterministic projection defined in [SDK, CLI, Platform Shell, and Library](sdk-cli.md#libraries-and-platform-integration), and each backend maps its native package identity to that same public identity.
- One native declaration per supported backend. These declarations state only what the backend needs to build and consume the library; they do not restate platform configuration.
- Platform packages shared by all backends. A library's Android, iOS, or other platform code is attached by the application's platform shell from the library graph, independently of the backend that compiled the C++ sources.

Public headers are the authoritative library interface.
A module interface a library offers names the same entities as its headers.
A library that offers only a module interface is usable only by backends with the C++ modules capability and is not in the standard form.

Every backend generates the same ordered library-graph record for the same declared dependencies: the public identity and resolved source root of each source-backed library, in dependency order, each once.
Platform shells consume that record to attach platform packages.
A library that declares several backends must project the same identity, resource namespace, and dependencies under each; a difference is a library defect that validation reports.

The framework dependency must communicate its public includes, minimum C++20 requirement, and required compile/link usage requirements.
Higher language modes are allowed when supported by the selected toolchain.
HuxerUI-managed C++ targets use standard language mode with extensions disabled.
Consumers use the public headers and APIs without implementation-only include paths.

The backend must preserve the complete dependency closure, including static libraries, system frameworks, required runtime libraries, and configuration-dependent link requirements.
Combining archives does not replace dependency propagation.

## Code generation and resources

`hcg` and `hrc` execute on the host and must be compatible with the selected HuxerUI distribution.
Each source requiring composable transformation is compiled through its transformed output exactly once, with the original source's include context preserved.
Generated files stay in build-owned output locations.
Changes to source files, resource inputs, tools, or relevant options must invalidate their generated outputs and dependent build steps.

Built-in, library, and application resources follow the existing [resource ordering, namespace, and package contract](resources.md).
The backend produces one final application resource package using the current `hrc` format, including the resources of transitive library dependencies; the platform integration owns embedding or staging it and its runtime discovery.
The package's runtime location on each platform is the same under every backend.
Framework libraries, resources, headers, and host tools must resolve consistently in every framework distribution form a backend declares.

## Artifacts

An application's binary or compiled-module basename defaults to its declared application target name.
Platform prefixes, extensions, and bundle formats apply to that basename: for a target named `gallery`, examples include `gallery.exe`, `gallery`, `gallery.app`, `libgallery.so`, and the Web modules `gallery.js` and `gallery.wasm`.
Android's default static-framework integration links HuxerUI into `lib<application-target>.so`, and its shell loads the actual application library reported by the build.
Bundle display names, application identifiers, and distribution-container filenames remain owned by platform configuration.

For the same declared target, platform, configuration, and naming settings, all backends must produce the same default application filenames.
Applications may explicitly override their output basename through native project settings.
Application overrides and any configuration postfixes must be reflected in generated artifact metadata; the shell must not require a second manual library-name setting.

`package` produces the same kind of distributable for a platform under every backend and publishes it below `dist/<platform>`.
Package publication validates the staged result before replacing the previous published output.

Published HuxerUI SDK framework artifacts use fixed basenames:

| Framework artifact | Basename | Platform filenames |
| --- | --- | --- |
| Static library | `huxerui_static` | `huxerui_static.lib` on Windows; `libhuxerui_static.a` on the other current platforms. |
| Shared library, where supported | `huxerui` | `huxerui.dll` on Windows, `libhuxerui.so` on Linux/Android, and `libhuxerui.dylib` on macOS. |

The iOS static XCFramework contains `libhuxerui_static.a` in each slice, and a Windows shared-framework import library uses `huxerui.lib`.
Filenames above are shown before any configuration postfix.
The current Web SDK archive is named `libhuxerui.a`; adopting this draft requires updating its packaging and consumers to `libhuxerui_static.a` together.
These names bind the SDK that publishes prebuilt framework libraries.
A backend that builds the framework from source inside the application's build owns the name of that intermediate, which no consumer observes.

## Outside the standard model

The contract states what a build must provide, never where it comes from.
The following are backend-owned and are not differences between conforming backends:

- The compiler and linker, within the target platform's required ABI.
- Where the toolchain, the platform SDK, and system or third-party dependencies are acquired, within the platform's documented minimum versions.
- The linkage of the toolchain runtime and of system and third-party dependencies, which retain their own requirements.
- Internal target names, helper organization, compiler flag sequences, and cache directory layout.

Artifacts must match the selected target ABI, toolchain runtime, and declared deployment requirements; dependency acquisition may use the build system's native package manager.
A backend documents these choices in its own design document.

## Generated records

Backends reuse the existing project, ordered library-graph, and application-integration records where their semantics apply, and every backend emits them with the same schema.
Shared machine-readable records use JSON with an explicit schema version and describe:

- Project and application identity, plus ordered library identities and source roots needed by platform integration.
- Requested and effective configuration, target ABI, framework selection, standard settings, enabled capabilities, and deviations.
- Actual artifact paths and kinds, resource locations, and platform-specific bundle or library-loading information needed by the caller.

Artifact information comes from the configured targets or native tool outputs, including output-name and configuration-postfix changes.
A backend whose native tool already reports versioned machine-readable results may satisfy this through a translation owned by its HuxerUI integration; the native tool is not required to emit HuxerUI's schema.
The CLI consumes these results instead of guessing filenames, reading human-oriented tool output, or parsing build scripts for target declarations.
An unsupported schema version or a missing required artifact must produce a diagnostic.
Changes to target, toolchain, framework selection, profile, or effective options must trigger the appropriate reconfiguration or rebuild.

## Conformance

A backend declares its supported hosts, targets, framework distribution forms, and capabilities.
Conformance is judged against this document, not against another backend.

**Record equivalence.** For the same project content, every backend that can build it generates equal project and library-graph records.

**Default model.** A default build is checked for the standard facts this document defines: static framework linkage, platform entry and library loading, resource package order, namespaces and runtime location, default artifact names, the package kind, and its publication location.
A difference in a standard fact is permitted only when this section lists it with its reason.
No difference is currently listed.

**Minimum acceptance.** A backend's declared support is checked with:

- CLI and direct native builds using equivalent project settings and producing equivalent application behavior.
- Clean and incremental builds in declared framework distribution forms and Debug/Release configurations.
- Declared capability choices, including explicit `off`, inherited defaults, and unsupported-option diagnostics.
- A library dependency with external link requirements, required platform entry points, and application/library resources, including a transitive library.
- Source and resource changes; default artifact names, explicit output-name overrides, and configuration postfixes where the backend has them; paths containing spaces; multiple configurations or ABIs where supported.
- Installation or package relocation, startup, resource loading, and validation of the packaged native artifacts on available target environments.

Unavailable hosts or devices are reported separately from passed checks.
Acceptance checks observable outcomes; internal target names, helper organization, and compiler flag sequences may differ across backends.

## Adoption

This draft changes how existing behavior is described before it changes behavior, and each item below is remaining work rather than current state.

Shared:

- The CLI gains a backend dimension beside its platform drivers and dispatches `build`, `run`, and `package` by project backend; the separate `huxerui mcpp build` frontend is replaced by that dispatch.
- The project, library-graph, application-integration, and artifact records receive explicit schema versions.
- Android and iOS shell templates isolate their backend invocation so the rest of the shell is identical across backends.
- Ownership statements in [SDK, CLI, Platform Shell, and Library](sdk-cli.md) that name CMake as the owner of common sources, resources, targets, and the library graph are restated for the project's backend, with the CMake mapping kept as that backend's description.

CMake:

- Application launch and artifact metadata, currently produced by three platform-specific mechanisms, are reported through the shared record schema.
- Uniform framework-linkage control and the Web SDK archive rename described above.

mcpp:

- The integration currently builds desktop applications and packages them itself; conforming means producing the application core for the standard platform shells and generating the shared records.
- Library resources and platform packages of transitive dependencies must reach the final resource package and the library graph.
- Library templates generate the native declarations for each backend the library supports.
