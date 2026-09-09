// A stateful composable. hcg rewrites this file's [[huxerui::composable]]
// functions; the entry file is left alone, which is why the composable lives
// here rather than beside main().

#include "counter.h"

using namespace huxerui;

[[huxerui::composable]]
View Counter() {
  auto count = UseState(0);

  return Row {
    Button("Count").OnClick([count] { count += 1; }),
    Text(count).With(FontSize(24.0F)),
  }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center));
}
