# Build System Conformance Test Suite

The suite checks that a build-system integration produces the standard model of the [Build System Integration Contract](../specs/build-system-integration.md).
It consists of real HuxerUI projects, the facts each project must produce, and a runner that builds them with one integration and reads those facts from the generated records and the produced artifact.

No integration is the reference.
Each project states its expected facts once in `expected.json`, and every integration is checked against that file.

## Projects

Each project has one set of sources, resources, and platform shells, plus one native declaration per build system.

| Project | Checks |
| --- | --- |
| `app_minimal` | Default profile, default artifact name, static framework linkage, the final resource package and its runtime location, composable code generation, and resource resolution in a started application. |
| `app_library` | A library that depends on another library: the ordered library-graph record, public identities and derived resource namespaces, library and transitive resources in the final package, and their resolution at run time. |
| `app_modules` | The same program as `app_library` written only with C++ modules and no `#include`. It requires the `modules` capability and must produce the same library graph and resource package as `app_library`. |

A project lists the capabilities it needs in `requires`.

## Checkpoints

The suite is divided into checkpoints so a run shows which part of the contract an integration meets.

| Checkpoint | Checked against `expected.json` | Compared between integrations |
| --- | --- | --- |
| `build` | `profile`, `artifact.name`, `artifact.bundle` | |
| `linkage` | `framework.linkage`, read from the artifact's dynamic dependencies; on Android also `android.exports` | |
| `resources` | `resources.location`: the final package where the Runtime looks for it | `resources.sha256`, `resources.files` |
| `libraries` | `libraries`: the ordered library-graph record | `libraries.roots` |
| `launch` | `launch.*`: what the started application resolved | |

The compared facts are also checked between a project and the project it names in `sameModelAs`.
A difference is accepted only when [`allowed-differences.json`](allowed-differences.json) lists it with its reason.
The list is empty.

## Declared support

Each integration states what it supports in `buildtools/<integration>/support.json`: for every target platform, the hosts that can build it, the checkpoints it implements, and its capabilities.
The suite checks an integration for exactly that.
An undeclared target, host, checkpoint, or capability is reported as `undeclared` and skipped; an environment that cannot run a check, such as a machine without a display, is reported as `unavailable`.
Neither is a pass or a failure.

## Running

```bash
python3 buildtools/cts/cts.py list
python3 buildtools/cts/cts.py build --integration cmake --target linux --out /tmp/cts
python3 buildtools/cts/cts.py build --integration mcpp --target linux --mcpp-toolchain llvm@22.1.8 --out /tmp/cts
python3 buildtools/cts/cts.py check --integration mcpp --target linux --check resources libraries --out /tmp/cts
python3 buildtools/cts/cts.py compare --check resources /tmp/cts/results
python3 buildtools/cts/cts.py matrix /tmp/cts/results
```

`build` builds the projects and records their facts; `check` evaluates the named checkpoints, or all of them when none is named; `run` does both.
`--project` limits any of them to the named projects.
`compare` checks the facts integrations must agree on, and `matrix` prints the build status of every project and integration.
Every command exits non-zero when something it checked failed.
The `launch` checkpoint starts each application with `HUXERUI_CTS_PROBE` set and reads what it recorded.

## Targets

| Target | What is checked |
| --- | --- |
| `linux`, `windows` | The program, its dynamic dependencies, and the package in `<program>.resources/`; the application is started. |
| `macos` | The application bundle, the program inside it, and the package in the bundle's resources; the application is started. |
| `web` | The ES module and WebAssembly module, and the package preloaded into the module's data file. |
| `android` | The application core the platform shell consumes: `lib<target>.so` for `arm64-v8a`, its exported JNI entries, and the final resource package. |

The Android checks stop at the application core; building the APK through the Gradle shell is not yet a checkpoint.
iOS is not covered yet.

`.github/workflows/build-system-conformance.yml` runs every checkpoint as its own step for each integration, then compares the integrations, on pull requests that touch a build system, the framework sources, or the suite.
