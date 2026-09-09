// The composable lives here rather than beside main() on purpose.
//
// mcpp's build programs can ADD a source but cannot replace one, so a
// transformed copy of the target's entry would link beside the original
// (`multiple definition of 'main'`). huxerui.rules therefore leaves the entry
// alone and owns every other source -- which is also why mcpp.toml says
// `sources = []`.
//
// AGENTS.md already asks that the app root not be annotated, so this is the
// arrangement the framework expects anyway.

#include "counter.h"

using namespace huxerui;

// A HAND-WRITTEN scope, using the public macro directly and never touched by
// hcg. It is here as evidence: macros do not cross a module boundary, so this
// compiles only because huxerui.rules force-includes the generated prelude.
View Banner() {
  HUXERUI_SCOPE({ return Text("hand-written HUXERUI_SCOPE"); });
}

[[huxerui::composable]]
View Counter() {
  auto count = UseState(0);

  return Column {
    Banner(),
    Row {
      Button("Count").OnClick([count] { count += 1; }),
      Text(count).With(FontSize(24.0F)),
    }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center)),
  }.With(Spacing(8.0F));
}
