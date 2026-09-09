# The mcpp leg of the HuxerUI build

HuxerUI has two build systems. **CMake is the full-platform one and the only
path for Android, iOS and Web**; mcpp builds the framework and applications on
Linux, Windows and macOS. Neither reads the other's files, and deleting this
directory plus `mcpp.toml` and `build.mcpp` returns the repository to a
CMake-only project.

The design, the phasing and the open questions are in
[`.agents/docs/2026-09-09-mcpp-native-build-and-modules-plan.md`](../.agents/docs/2026-09-09-mcpp-native-build-and-modules-plan.md);
the analysis behind it is in [`docs/design/mcpp-dual-build-analysis.md`](../docs/design/mcpp-dual-build-analysis.md).

## Building

```bash
mcpp build                          # the framework
cd examples/mcpp_demo && mcpp build # an application on top of it
```

On Linux the framework needs the distribution's GTK 4 stack, exactly as the
CMake build does:

```bash
sudo apt-get install -y libgtk-4-dev libepoxy-dev libsoup-3.0-dev pkg-config
```

## What lives here

| Path | Contents |
|---|---|
| `rules/` | `huxerui.rules`, the module a consumer's `build.mcpp` imports |
| `parity/check_parity.py` | asserts mcpp.toml and the CMake build describe the same project |

`../mcpp.toml` is the framework package and the workspace root; `../build.mcpp`
resolves the Linux platform dependencies and produces the builtin resource
header.

## Writing an application

The manifest names one dependency and the build program makes one call:

```toml
[dependencies]
huxerui = { path = "../.." }        # or a version, once published
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
never restates them. `examples/mcpp_demo/` is the worked example.

## Two things worth knowing before editing

**The host tools are the committed ones.** `hcg` and `hrc` are resolved from
`tools/prebuilt/<platform>/<arch>/`, which is the same binary the CMake build
runs. That is deliberate: building them from source on the mcpp side would let
the two build systems generate different code from the same input.

**The builtin resources are compiled twice.** The framework compiles them for
the header its own 11 translation units `#include`; an application compiles
them again for the package it merges with its own resources. mcpp has no
channel from a dependency's build output to a consumer's build input -- a
consumer is given `dep_dir()`, the dependency's *source* root -- and inventing
one would mean writing into a package root that may be read-only. 44 files /
196 KB, incrementally cached; the alternatives are enumerated in the plan.

## Parity

Two build systems maintaining one set of facts drift, and the drift is quiet:
add `platform/windows/win32_foo.cpp`, forget `mcpp.toml`, and the Windows mcpp
build fails at link with `undefined reference`, far from the cause.

```bash
python3 mcpp/parity/check_parity.py
```

runs in seconds, needs no toolchain, and is the first job in
`.github/workflows/mcpp-build.yml`. It already caught one real defect: a bare
`platform/windows/*.cpp` glob swept in `windows_installer.cpp`, which is the
WiX custom-action DLL built separately by `cmake/HuxerUISdk.cmake`, not part of
the framework.
