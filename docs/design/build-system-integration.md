# Build System Integration Contract

> Status: Draft for review. This proposes common behavior for HuxerUI build-system integrations; it does not claim that every current workflow implements it.

CMake is the first integration to implement this contract; mcpp, xmake, and other build systems may implement the same observable contract through their own project formats, dependency resolution, and target APIs.
Conformance applies to each integration's declared host/target combinations and capabilities, and is judged against this contract rather than against another integration.
The current implementations remain documented in [SDK, CLI, Platform Shell, and Library](sdk-cli.md) and [mcpp Build System](mcpp-build-system.md).

## Responsibilities

| Owner | Responsibility |
| --- | --- |
| HuxerUI CLI | Select the project, build system, platform, SDK/source, and device; pass explicit options; orchestrate build, launch, and package publication. |
| Build-system integration | Resolve project settings, dependencies, and tools; generate code and resources; compile and link targets; report effective settings and actual artifacts. |
| Platform shell and tools | Own application lifecycle, platform metadata, permissions, signing, installation, launch, and distribution containers. |

Projects keep their build system's native configuration as the source of truth.
A project built with an integration's defaults follows the defaults in this contract.
A result that departs from them exists only because the developer selected it explicitly, in native project configuration or on the command line, and the integration reports it in its effective settings.
Detailed or non-standard configuration may use the build system's native files and commands directly.
Direct native builds and CLI-driven builds must use the same target, code-generation, resource, and dependency rules.
Any shared request or result records are generated metadata, not another user-maintained project manifest.

## CLI build intent

`build` produces application artifacts; `run` builds the matching configuration and launches it on the selected destination; `package` builds and publishes the platform's distributable output.
The default logical profile is `debug` for `build` and `run`, and `release` for `package`.
The CLI reports exit code `0` for success, `2` for invalid command usage, and `1` for an operation failure, preserving the failing step and underlying tool diagnostics.

### Build-system selection and dispatch

A project has one build system, identified by the native build file at its root.
The CLI offers one set of verbs and dispatches each to the project's integration; it exposes no command named after a build system.
A root that declares more than one build system requires an explicit selection, and the CLI diagnoses the ambiguity instead of choosing.
An option that does not apply to the project's build system is rejected with the reason and never ignored.

`huxerui create` selects the build system from the requested project style unless one is named: a header-based project defaults to CMake and a C++ modules project defaults to mcpp.
This is a creation default; it does not bind a language style to a build system.

The common sources, the `resources/` root, and the `platform/<platform-id>` shells have the same layout and meaning under every integration, so the following work needs no build-system-specific knowledge:

- Creating a project and adding platforms.
- Building, running, packaging, and testing through the CLI.
- Editing common sources and resources.
- Editing identity, permissions, icons, signing, and other platform metadata in the platform shells.

Declaring a library dependency is one statement in the native build file.
The CLI may delegate that edit to a build system's own dependency command where one exists; it never rewrites a native build script as text.
A capability that only one build system offers is used through that build system's native configuration or commands and is not wrapped by the CLI.

The CLI currently dispatches `build`, `run`, and `package` for CMake projects and offers `huxerui mcpp build` as a separate frontend; this draft replaces that frontend with dispatch.

The shared request carries the following information where applicable:

| Information | Meaning |
| --- | --- |
| Operation and project | Build, run, or package; project root, application identity, and selected build system. |
| Host and target | Host OS and architecture for executable tools; target platform, optional canonical ABI list, platform SDK, and deployment minimum for produced binaries. |
| Framework selection | Selected HuxerUI SDK/package or source checkout, including its resolved version. |
| Settings | Logical profile and explicit optimization, LTO, and framework-linkage choices. |
| Destination and output | Selected device or simulator, incremental build location, and requested publication location. |

Integrations map this intent to native commands and settings; compiler flags are implementation details of that mapping.
Native configuration names may differ from logical profiles, as with an Android Release variant using a CMake `RelWithDebInfo` configuration.
The request's transport and command-line spelling must be agreed when wiring an integration into the CLI; this draft specifies their meaning.

The CLI's `--abi` option selects one or more ABIs for a single target platform. The CLI resolves aliases to that platform's canonical spellings and passes a deduplicated list to the build-system integration. An absent list preserves native project defaults. Integrations must reject unsupported explicit selections and keep incremental outputs for different ABI selections distinct where the native tools require it.

Report which ABIs each produced artifact contains. Integrations may produce one artifact with several ABIs where the platform supports it, or separate artifacts with unambiguous ABI-specific locations. The requested selection must be satisfied in full.

## Common settings

Explicit CLI values take precedence over matching project options, followed by integration defaults.
An omitted value preserves project configuration; explicit `off` and explicit `default` remain distinguishable from omission.
A framework version pinned by native project configuration or its lock file remains project policy; a CLI framework selection that the integration cannot honor without changing that pin is rejected.
These settings govern integration-managed source targets and build steps; independent project toolchain customization remains project-owned.

| Setting | Values and default | Required meaning |
| --- | --- | --- |
| Profile | `debug`, `release`; operation-dependent default above | Select development or optimized application configuration independently of the options below. |
| Optimization | `default`, `size`, `minsize`; `default` | Keep the native profile's optimization level, prefer code size, or select a stronger size mode supported by the toolchain. |
| LTO | `on`, `off`; `off` | Enable or disable integration-managed link-time optimization in release configurations. |
| Framework linkage | `static`, `shared`; applications default to `static` on every platform | Select the HuxerUI framework's linkage, independently of system and third-party dependency linkage. |

Optimization and LTO choices apply to release configurations; they do not enable release optimization in Debug.
Integrations must diagnose an unsupported explicit choice rather than silently substitute another mode.
They must report effective settings and their scope: prebuilt libraries retain their published settings, while source targets can be rebuilt with overrides.
Size optimization preserves the framework's required exception, RTTI, threading, ownership, and lifecycle behavior.

The current CMake mappings are `HUXERUI_OPTIMIZATION` and `HUXERUI_ENABLE_LTO`.
`HUXERUI_BUILD_SHARED` and `HUXERUI_BUILD_STATIC` select available source framework artifacts; the consuming target selects its link form.
The CLI currently exposes optimization and LTO overrides; uniform framework-linkage control is proposed by this draft.

## Linkage and artifact names

Applications use static HuxerUI linkage by default on every supported platform and build-system integration.
Shared framework linkage requires an explicit selection and declared platform support; an unavailable link form produces a diagnostic.
SDKs and framework source builds may provide both forms, while application consumption follows the same static default.
System and third-party dependencies retain their own linkage requirements.

Published framework artifact basenames are fixed across integrations:

| Framework artifact | Basename | Platform filenames |
| --- | --- | --- |
| Static library | `huxerui_static` | `huxerui_static.lib` on Windows; `libhuxerui_static.a` on the other current platforms. |
| Shared library, where supported | `huxerui` | `huxerui.dll` on Windows, `libhuxerui.so` on Linux/Android, and `libhuxerui.dylib` on macOS. |

The iOS static XCFramework contains `libhuxerui_static.a` in each slice, and a Windows shared-framework import library uses `huxerui.lib`.
Filenames above are shown before any configuration postfix.
The current Web SDK archive is named `libhuxerui.a`; adopting this draft requires updating its packaging and consumers to `libhuxerui_static.a` together.
These names bind framework libraries that an SDK or package publishes for consumers; an integration that builds the framework from source within the application's own build owns the name of that intermediate.

An application's binary or compiled-module basename defaults to its declared application target name.
Platform prefixes, extensions, and bundle formats apply to that basename: for a target named `gallery`, examples include `gallery.exe`, `gallery`, `gallery.app`, `libgallery.so`, and the Web modules `gallery.js` and `gallery.wasm`.
Android's default static-framework integration links HuxerUI into `lib<application-target>.so`, and its shell loads the actual application library reported by the build.
Bundle display names, application identifiers, and distribution-container filenames remain owned by platform configuration.

For the same declared target, platform, configuration, and naming settings, all integrations must produce the same default application and published framework filenames.
Applications may explicitly override their output basename through native project settings.
Application overrides and any configuration postfixes must be reflected in generated artifact metadata; the shell must not require a second manual library-name setting.

## Platform shells and the application core

Platform shells are part of the standard project model.
A shell is created with the project, lives in version control, and owns the same platform facts under every integration, so platform metadata, signing, and platform extension points are configured once and survive a change of build system.
The integration's output toward the shell is the application core:

| Platform | Shell | Application core produced by the integration |
| --- | --- | --- |
| Android | Gradle application project; owns the application ID, SDK levels, ABI selection, manifest, Java or Kotlin sources, dependencies, signing, and the APK. | `lib<application-target>.so` for each requested ABI with HuxerUI linked in, the final resource package, and one application-integration record per ABI naming the actual library. |
| iOS | Xcode application project; owns the process entry, Info.plist, launch screen, asset catalog, capabilities, signing, and the application bundle. | The application-core static archive, its linker inputs, and the final resource package at a stable location for the selected destination. |
| Windows, macOS, Linux | Source inputs under `platform/<platform-id>`: the process entry and the platform's manifest or bundle metadata. | The application executable or bundle, the final resource package at its runtime location, and the installation content from which the platform's distribution container is produced. |
| Web | The HTML entry and host-element mount code. | The application's ES module and WebAssembly module, with the final resource package available to the Runtime. |

A shell that is a platform build project invokes the project's integration to produce the application core and consumes only the generated records and the artifacts they name.
That invocation is the only part of a shell that differs between integrations.
The shell does not assume a filename, and the integration does not read or duplicate shell configuration such as application identifiers, SDK levels, permissions, or signing.

Producing a platform package without its standard shell is a capability of the integration that offers it.
It requires an explicit selection, and its result is not a default build.

Open question: the current Android shell reaches CMake through Gradle's `externalNativeBuild`, which accepts only CMake and ndk-build. Invoking another integration needs a Gradle task that supplies the application core to the same variant, and this has not been prototyped.

## Targets and dependencies

The framework dependency must communicate its public includes, minimum C++20 requirement, and required compile/link usage requirements.
Higher language modes are allowed when supported by the selected toolchain; C++ module support is a separately declared capability.
HuxerUI-managed C++ targets use standard language mode with extensions disabled.
Consumers use the public headers and APIs without implementation-only include paths.

The integration must preserve the complete dependency closure, including static libraries, system frameworks, required runtime libraries, and configuration-dependent link requirements.
Combining archives does not replace dependency propagation.
Artifacts must match the selected target ABI, toolchain runtime, and declared deployment requirements; dependency acquisition may use the build system's native package manager.
The compiler, the linker, and the source of toolchains and dependencies are integration-owned within the platform's required ABI and minimum versions; they are not differences between conforming integrations.

Entry points discovered by the platform must survive archive extraction, dead stripping, and LTO.
On Android, static framework integration retains the required named JNI entries and loads the application's actual shared-library name before HuxerUI Java services are used.
Libraries retain ownership of their initialization; linking archives does not combine multiple `JNI_OnLoad` implementations.

## Libraries

A HuxerUI library has one standard form, so one library source tree serves every integration it declares:

- Public headers under `include/`, implementation under `src/`, optional resources under `resources/`, and optional platform packages under `platform/<platform-id>`.
- One public identity. The implementation name, resource namespace, and primary header derive from it through the deterministic projection in [SDK, CLI, Platform Shell, and Library](sdk-cli.md#libraries-and-platform-integration), and each integration maps its native package identity to that same public identity.
- One native declaration per supported build system. It states the public identity, sources, public includes, resource root, dependencies on other libraries, and external link requirements; it does not restate platform configuration.
- Platform packages shared by all integrations. A library's Android, iOS, or other platform code is attached by the application's platform shell from the library graph, independently of the integration that compiled its C++ sources.

Public headers are the authoritative library interface.
A module interface that a library offers names the same entities as its headers.
A library that offers only a module interface is usable only by integrations that declare C++ module support and is outside the standard form.

An application declares a library dependency with one native statement naming the public identity and its local path or pinned source revision.
Every integration then generates the same ordered library-graph record for the same declarations: the public identity and resolved source root of each source-backed library, following dependencies depth-first in declaration order, each library once.
Platform shells consume that record to attach platform packages, and the final resource package includes each library's resources in the same order, transitive dependencies included.

A library that declares several build systems must project the same identity, resource namespace, and dependencies under each.
A difference is a library defect, and validation reports it by comparing the records each integration generates.

Open question: an mcpp build program currently sees only its direct dependencies. Whether mcpp's own machine-readable output provides the full ordered graph has not been confirmed.

## Code generation and resources

`hcg` and `hrc` execute on the host and must be compatible with the selected HuxerUI distribution.
Each source requiring composable transformation is compiled through its transformed output exactly once, with the original source's include context preserved.
Generated files stay in build-owned output locations.
Changes to source files, resource inputs, tools, or relevant options must invalidate their generated outputs and dependent build steps.

Built-in, library, and application resources follow the existing [resource ordering, namespace, and package contract](resources.md).
The integration produces one final application resource package using the current `hrc` format; the platform integration owns embedding or staging it and its runtime discovery.
Framework libraries, resources, headers, and host tools must resolve consistently in both supported SDK and source-checkout workflows.

## Generated results

Reuse the existing project, ordered library-graph, and application-integration records where their semantics apply.
Shared machine-readable records use JSON with an explicit schema version and describe:

- Project and application identity, plus ordered library identities and source roots needed by platform integration.
- Requested and effective configuration, target ABI, framework selection, and applicable common settings.
- Actual artifact paths and kinds, resource locations, and platform-specific bundle or library-loading information needed by the caller.

Artifact information comes from the configured targets or native tool outputs, including output-name and configuration-postfix changes.
The CLI consumes these results instead of guessing filenames or parsing build scripts for target declarations.
An integration may produce these records by translating its native tool's versioned machine-readable output; human-oriented tool output is not a source.
An unsupported schema version or a missing required artifact must produce a diagnostic.
Changes to target, toolchain, framework selection, profile, or effective options must trigger the appropriate reconfiguration or rebuild; cache directory layout remains build-system-owned.
Package publication validates the staged result before replacing the previous published output.

## Conformance

An integration declares its supported hosts, targets, framework distribution forms, and capabilities.
Conformance is judged against this contract with a shared fixture project, not by comparing one integration's output with another's.

**Record equivalence.** For the same project content, every integration that can build it generates equal project and library-graph records.

**Default model.** A default build, meaning a project from `huxerui create` built, run, or packaged with no option beyond platform and destination, is checked for these facts, read from the generated records and the produced artifacts:

- The profile is `debug` for `build` and `run` and `release` for `package`, and the effective settings report no optimization override, no LTO, and no explicit departure from a default.
- HuxerUI is linked statically into the application; on Android the framework is inside `lib<application-target>.so` and the shell loads the library named by the record.
- One final resource package exists, with built-in, library, and application resources in the contract's order and namespaces, at the platform's runtime location.
- The application's binary or modules carry the default basename from the declared application target.
- `package` produces the platform's standard distributable kind and publishes it below `dist/<platform>`.

A difference in one of these facts is permitted only when this section lists it with its reason.
No difference is currently listed.

**Minimum acceptance.** An integration's declared support is checked with:

- CLI and direct native builds using equivalent project settings and producing equivalent application behavior.
- Clean and incremental builds in declared SDK/source modes and Debug/Release configurations.
- Supported optimization, LTO, and link-form choices, including explicit `off`, inherited defaults, and unsupported-option diagnostics.
- A library dependency with external link requirements, required platform entry points, and application/library resources.
- Source and resource changes; default artifact names, explicit output-name overrides, and configuration postfixes; paths containing spaces; multiple configurations or ABIs where supported.
- Installation or package relocation, startup, resource loading, and validation of the packaged native artifacts on available target environments.

Unavailable hosts or devices are reported separately from passed checks.
Acceptance checks observable outcomes; internal target names, helper organization, and compiler flag sequences may differ across build systems.
