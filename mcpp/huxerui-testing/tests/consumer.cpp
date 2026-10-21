import std;
import huxerui;
import huxerui.testing;

huxerui::View Counter() {
  auto count = huxerui::UseState(0);
  return huxerui::Column{
      huxerui::Text(std::to_string(count.Get())).Key("count"),
      huxerui::Button("Increment").OnClick([count] { count += 1; }),
  };
}
int main() {
  huxerui::Application app(Counter, {.show_debug_overlay = false});
  huxerui::testing::UiTest ui(app);
  ui.Find(huxerui::testing::UiSelector::Text("Increment")).Tap();
  if (ui.Find(huxerui::testing::UiSelector::Key("count")).One().text != "1") return 1;
  ui.Pump(std::chrono::milliseconds(100));
  if (std::abs(ui.Now() - 0.1) > 0.0001) return 2;
  std::cout << "Public windowless module consumer: passed\n";
}
