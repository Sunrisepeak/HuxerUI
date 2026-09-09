# HuxerUI mcpp demo

A HuxerUI application built natively by mcpp. It is deliberately outside the
CMake example list: `examples/CMakeLists.txt` does not add it, so it exercises
the mcpp leg and nothing else.

```bash
mcpp build
mcpp run
```

On Linux the framework needs the distribution's GTK 4 stack, exactly as the
CMake build does:

```bash
sudo apt-get install -y libgtk-4-dev libepoxy-dev libsoup-3.0-dev pkg-config
```

No separate CMake build of HuxerUI is required. `mcpp.toml` names the framework
as a path dependency and mcpp builds it as part of this build.

## What this demonstrates

The whole manifest is:

```toml
[dependencies]
huxerui = { path = "../.." }
```

and the whole build program is:

```cpp
import mcpp;
import huxerui.rules;
int main() { return huxerui::rules::configure({ .resources = "resources" }) ? 0 : 1; }
```

The previous revision of this demo spelled out an SDK include directory, the
HuxerUI static library, twenty absolute `/lib64/*.so` paths and a runtime
`library_dirs` entry, and its README recorded that "stateful composables and
packaged resources need an additional mcpp integration layer".

`huxerui.rules` is that layer, so the page now uses a `[[huxerui::composable]]`
function with `UseState` and ships a resource package. Both are scheduled as
build-graph edges rather than done in the build program, so they are
incremental, parallel and attributable to the file that failed.

GTK reaches the link line without this manifest naming it: a dependency's
`build.mcpp` emits `link-lib` / `link-search` that reach the **final** link, and
HuxerUI's resolves the GTK stack through pkg-config -- the same `.pc` files
`cmake/platform/Linux.cmake` reads.

## Scope

Linux, Windows and macOS. Android, iOS and Web are CMake-only: mcpp's target
table has no rows for them, and `modules/toolchain-model/src/triple.cppm`
declines `androideabi` and `wasi` as outside its target language. See
`.agents/docs/2026-09-09-mcpp-native-build-and-modules-plan.md`.
