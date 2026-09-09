// The same page, written against the C++20 module front door.
//
// `import huxerui;` replaces `#include <huxerui/huxerui.h>` and NOTHING ELSE
// changes: the names, the DSL and [[huxerui::composable]] are identical,
// because modules/huxerui.cppm includes those very headers in its global
// module fragment and re-exports what they declare. Same entities, same
// linkage, same library.

#include <cstdio>

import huxerui;

#include "counter.h"

using namespace huxerui;

View App() {
  return Column {
    Text("mcpp + HuxerUI", TextRole::Title),
    Text("Built natively by mcpp, consumed as a C++20 module."),
    Divider(),
    Counter(),
    Row {
      Button("Say hello").OnClick([] { std::puts("Hello from the HuxerUI mcpp demo."); }),
    }.With(Spacing(12.0F)),
  }.With(
      Padding(32.0F),
      Spacing(16.0F),
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(Color::Rgb(248, 249, 252))
  );
}

const Application application{
    App,
    {
        .window = {
            .title = "mcpp HuxerUI Demo",
            .initial_size = {640.0F, 420.0F},
        },
    },
};

int main() {
  return RunApplication();
}
