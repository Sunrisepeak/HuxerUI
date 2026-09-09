// A module interface unit. No header, no #include of the framework -- the
// project is module-style throughout.
//
// The global module fragment carries <typeinfo> because GCC's `typeid` check
// is per-translation-unit and HuxerUI's UseState() instantiates typeid in the
// caller. huxerui.rules cannot force this include for a package that has
// module units: `-include` prepends before `module;`, which is ill-formed.

module;

#include <typeinfo>

export module counter;

import huxerui;

using namespace huxerui;

[[huxerui::composable]]
export View Counter() {
  auto count = UseState(0);

  return Row {
    Button("Count").OnClick([count] { count += 1; }),
    Text(count).With(FontSize(24.0F)),
  }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center));
}
