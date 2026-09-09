// This page deliberately uses a [[huxerui::composable]] function with UseState.
//
// That is the part the previous revision of this demo could not do: its README
// recorded that "stateful composables and packaged resources need an additional
// mcpp integration layer". `huxerui.rules` is that layer, so the demo now
// exercises it -- if the composable transform were not scheduled, this file
// would fail to compile with "Use...() must be called from a
// [[huxerui::composable]] function".

#include <huxerui/huxerui.h>

#include <cstdio>

using namespace huxerui;

[[huxerui::composable]]
View Counter() {
  auto count = UseState(0);

  return Row {
    Button("Count").OnClick([count] { count += 1; }),
    Text(count).With(FontSize(24.0F)),
  }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center));
}

View App() {
  return Column {
    Text("mcpp + HuxerUI", TextRole::Title),
    Text("Built natively by mcpp: one dependency line, one build.mcpp line."),
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
