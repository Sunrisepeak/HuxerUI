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
| `app_modules` | The same program as `app_library` written with C++ modules. It requires the `modules` capability and must produce the same library graph and resource package as `app_library`. |

A project lists the capabilities it needs in `requires`.
An integration that does not declare them, or does not declare the platform, is reported as `undeclared` rather than failed.

## Facts

| Fact | Source | Checked against |
| --- | --- | --- |
| `profile` | Generated record | `expected.json` |
| `artifact.name` | Produced artifact | `expected.json` |
| `framework.linkage` | The artifact's dynamic dependencies | `expected.json` |
| `resources.location` | The final package beside the artifact | `expected.json` |
| `libraries` | The library-graph record | `expected.json` |
| `launch.*` | What the started application resolved | `expected.json` |
| `resources.sha256`, `resources.files`, `libraries.roots` | Final package and library-graph record | The other integrations, and the project named by `sameModelAs` |

A difference is accepted only when [`allowed-differences.json`](allowed-differences.json) lists it with its reason.
The list is empty.

## Running

```bash
python3 buildtools/cts/cts.py run --integration cmake --platform linux --launch --out /tmp/cts
python3 buildtools/cts/cts.py run --integration mcpp --platform linux --launch --mcpp-toolchain llvm@22.1.8 --out /tmp/cts
python3 buildtools/cts/cts.py compare /tmp/cts/results
```

`run` exits non-zero when a project fails.
`--launch` starts each application with `HUXERUI_CTS_PROBE` set and compares what it recorded; without a display the launch check is reported as unavailable and the other facts are still checked.
`compare` prints the result matrix and checks the facts that integrations must agree on.

The suite currently collects facts on Linux.
Other platforms are reported as unavailable until their fact collection is added.

`.github/workflows/build-system-conformance.yml` runs both integrations and the comparison on pull requests that touch a build system, the framework sources, or the suite.
