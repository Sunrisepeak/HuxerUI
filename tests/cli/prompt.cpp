#include "support.h"

#include "prompt.h"

using namespace huxerui::cli::prompt;

namespace {

std::vector<Choice> BuildSystems() {
  return {{"cmake", ""}, {"mcpp", "C++ modules"}};
}

KeyEvent Press(Key key) {
  return {key, {}};
}

KeyEvent Type(std::string text) {
  return {Key::Text, std::move(text)};
}

std::vector<KeyEvent> Decode(std::string_view bytes) {
  KeyDecoder decoder;
  std::vector<KeyEvent> events;
  for (const char byte : bytes) {
    if (std::optional<KeyEvent> event = decoder.Feed(static_cast<unsigned char>(byte))) {
      events.push_back(std::move(*event));
    }
  }
  return events;
}

} // namespace

TEST_CASE("HuxerUICliPromptSelectMovesAndWraps") {
  State state = MakeSelect("Build system", BuildSystems(), 0);
  Apply(state, Press(Key::Up));
  REQUIRE(state.cursor == 1);
  Apply(state, Press(Key::Down));
  REQUIRE(state.cursor == 0);
  Apply(state, Press(Key::Down));
  Apply(state, Press(Key::Enter));
  REQUIRE(state.status == Status::Submitted);
  REQUIRE(Answer(state) == "mcpp");
  Apply(state, Press(Key::Up));
  REQUIRE(state.cursor == 1);
}

TEST_CASE("HuxerUICliPromptSelectRendersCursorAndHints") {
  const State state = MakeSelect("Build system", BuildSystems(), 1);
  const std::vector<std::string> lines = Render(state);
  REQUIRE(lines.size() == 4);
  REQUIRE(lines[0] == "? Build system");
  REQUIRE(lines[1] == "    cmake");
  REQUIRE(lines[2] == "  > mcpp  (C++ modules)");
}

TEST_CASE("HuxerUICliPromptMultiSelectRequiresOneChoice") {
  State state = MakeMultiSelect("Build systems", BuildSystems(), std::vector<std::size_t>{0, 1});
  Apply(state, Press(Key::Space));
  Apply(state, Press(Key::Down));
  Apply(state, Press(Key::Space));
  Apply(state, Press(Key::Enter));
  REQUIRE(state.status == Status::Active);
  REQUIRE(Render(state).back() == "  ! Select at least one option.");
  Apply(state, Press(Key::Space));
  REQUIRE(Render(state).back().find('!') == std::string::npos);
  Apply(state, Press(Key::Enter));
  REQUIRE(state.status == Status::Submitted);
  REQUIRE(CheckedIndices(state) == std::vector<std::size_t>{1});
  REQUIRE(Render(state) == std::vector<std::string>{"? Build systems: mcpp"});
}

TEST_CASE("HuxerUICliPromptTextUsesDefaultAndValidates") {
  State state = MakeText("Project name", "my-app", [](std::string_view value) {
    return value.find(' ') == std::string_view::npos ? std::string{} : std::string("No spaces.");
  });
  Apply(state, Type("a"));
  Apply(state, Press(Key::Space));
  Apply(state, Type("b"));
  Apply(state, Press(Key::Enter));
  REQUIRE(state.status == Status::Active);
  REQUIRE(state.error == "No spaces.");
  Apply(state, Press(Key::Backspace));
  Apply(state, Press(Key::Backspace));
  Apply(state, Press(Key::Backspace));
  REQUIRE(state.text.empty());
  Apply(state, Press(Key::Enter));
  REQUIRE(state.status == Status::Submitted);
  REQUIRE(Answer(state) == "my-app");
}

TEST_CASE("HuxerUICliPromptTextBackspaceRemovesOneCodePoint") {
  State state = MakeText("Name", "");
  Apply(state, Type("a"));
  Apply(state, Type("\xC3\xA9"));
  Apply(state, Press(Key::Backspace));
  REQUIRE(state.text == "a");
}

TEST_CASE("HuxerUICliPromptCancelEndsEveryKind") {
  State select = MakeSelect("Q", BuildSystems(), 0);
  Apply(select, Press(Key::Cancel));
  REQUIRE(select.status == Status::Cancelled);
  REQUIRE(Render(select) == std::vector<std::string>{"? Q: cancelled"});
  State text = MakeText("Q", "x");
  Apply(text, Press(Key::Cancel));
  REQUIRE(text.status == Status::Cancelled);
}

TEST_CASE("HuxerUICliPromptDecodesTerminalBytes") {
  const std::vector<KeyEvent> events = Decode("\x1b[A\x1b[B\x1bOA \r\x7fq\xC3\xA9\x03\x1b[1;5C");
  REQUIRE(events.size() == 9);
  REQUIRE(events[0].key == Key::Up);
  REQUIRE(events[1].key == Key::Down);
  REQUIRE(events[2].key == Key::Up);
  REQUIRE(events[3].key == Key::Space);
  REQUIRE(events[4].key == Key::Enter);
  REQUIRE(events[5].key == Key::Backspace);
  REQUIRE(events[6].text == "q");
  REQUIRE(events[7].text == "\xC3\xA9");
  REQUIRE(events[8].key == Key::Cancel);
}

TEST_CASE("HuxerUICliKeyPrompterRedrawsAndReturnsTheChoice") {
  std::vector<KeyEvent> keys{Press(Key::Down), Press(Key::Enter)};
  std::size_t next = 0;
  std::ostringstream output;
  KeyPrompter prompter([&]() -> std::optional<KeyEvent> {
    return next < keys.size() ? std::optional<KeyEvent>(keys[next++]) : std::nullopt;
  }, output);
  const std::vector<Choice> choices = BuildSystems();
  REQUIRE(prompter.Select("Build system", choices, 0) == std::optional<std::size_t>(1));
  REQUIRE(output.str().find("\x1b[4F\x1b[J") != std::string::npos);
  REQUIRE(output.str().ends_with("? Build system: mcpp\n"));
}

TEST_CASE("HuxerUICliKeyPrompterTreatsEndOfInputAsCancel") {
  std::ostringstream output;
  KeyPrompter prompter([]() -> std::optional<KeyEvent> { return std::nullopt; }, output);
  REQUIRE_FALSE(prompter.Text("Name", "x", {}).has_value());
}

TEST_CASE("HuxerUICliLinePrompterReadsNumberedAnswers") {
  std::istringstream input("\n9\n2\n\n1, 2\nbad name\ngood\n");
  std::ostringstream output;
  LinePrompter prompter(input, output);
  const std::vector<Choice> choices = BuildSystems();
  REQUIRE(prompter.Select("Style", choices, 1) == std::optional<std::size_t>(1));
  REQUIRE(prompter.Select("Style", choices, 0) == std::optional<std::size_t>(1));
  REQUIRE(output.str().find("! Enter a number from 1 to 2.") != std::string::npos);
  const std::vector<std::size_t> defaults{0};
  REQUIRE(prompter.MultiSelect("Build systems", choices, defaults) == std::optional<std::vector<std::size_t>>({0}));
  REQUIRE(prompter.MultiSelect("Build systems", choices, defaults) ==
          std::optional<std::vector<std::size_t>>({0, 1}));
  const Validator validate = [](std::string_view value) {
    return value.find(' ') == std::string_view::npos ? std::string{} : std::string("No spaces.");
  };
  REQUIRE(prompter.Text("Name", "", validate) == std::optional<std::string>("good"));
  REQUIRE(output.str().find("! No spaces.") != std::string::npos);
  REQUIRE_FALSE(prompter.Text("Name", "", validate).has_value());
}
