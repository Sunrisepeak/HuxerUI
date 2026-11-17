#pragma once

#include <cstddef>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace huxerui::cli::prompt {

/// One option of a selection prompt.
struct Choice {
  std::string label;
  std::string hint;
};

enum class Key {
  Up,
  Down,
  Space,
  Enter,
  Backspace,
  Text,
  Cancel,
};

/// A decoded key press. `text` holds the typed UTF-8 sequence for `Key::Text`.
struct KeyEvent {
  Key key = Key::Text;
  std::string text;
};

enum class Kind {
  Select,
  MultiSelect,
  Text,
};

enum class Status {
  Active,
  Submitted,
  Cancelled,
};

/// Returns an empty string for an acceptable answer, or the message to show otherwise.
using Validator = std::function<std::string(std::string_view)>;

/// The complete state of one prompt. The terminal layer only feeds key events to `Apply` and draws `Render`.
struct State {
  Kind kind = Kind::Select;
  std::string question;
  std::vector<Choice> choices;
  std::size_t cursor = 0;
  std::vector<bool> checked;
  std::string text;
  std::string default_text;
  Validator validate;
  std::string error;
  Status status = Status::Active;
};

[[nodiscard]] State MakeSelect(std::string question, std::vector<Choice> choices, std::size_t initial);
[[nodiscard]] State MakeMultiSelect(std::string question, std::vector<Choice> choices,
                                    std::span<const std::size_t> initially_checked);
[[nodiscard]] State MakeText(std::string question, std::string default_text, Validator validate = {});

/// Advances the prompt by one key event.
void Apply(State& state, const KeyEvent& event);

/// The lines that show the prompt in its current state, without terminal control sequences.
[[nodiscard]] std::vector<std::string> Render(const State& state);

/// The submitted answer as text: the chosen labels separated by ", ", or the entered text.
[[nodiscard]] std::string Answer(const State& state);

/// The checked indices of a multi-select state, in choice order.
[[nodiscard]] std::vector<std::size_t> CheckedIndices(const State& state);

/// Decodes terminal input bytes, including VT escape sequences for the arrow keys, into key events.
class KeyDecoder final {
public:
  [[nodiscard]] std::optional<KeyEvent> Feed(unsigned char byte);

private:
  std::string pending_;
};

/// Asks questions. A missing answer means the user cancelled.
class Prompter {
public:
  virtual ~Prompter() = default;

  [[nodiscard]] virtual std::optional<std::size_t> Select(std::string_view question, std::span<const Choice> choices,
                                                          std::size_t initial) = 0;
  [[nodiscard]] virtual std::optional<std::vector<std::size_t>>
  MultiSelect(std::string_view question, std::span<const Choice> choices,
              std::span<const std::size_t> initially_checked) = 0;
  [[nodiscard]] virtual std::optional<std::string> Text(std::string_view question, std::string_view default_text,
                                                        const Validator& validate) = 0;
};

/// Runs each prompt through the state machine with keys from `next_key`, writing frames to `output`.
class KeyPrompter : public Prompter {
public:
  KeyPrompter(std::function<std::optional<KeyEvent>()> next_key, std::ostream& output);

  [[nodiscard]] std::optional<std::size_t> Select(std::string_view question, std::span<const Choice> choices,
                                                  std::size_t initial) override;
  [[nodiscard]] std::optional<std::vector<std::size_t>>
  MultiSelect(std::string_view question, std::span<const Choice> choices,
              std::span<const std::size_t> initially_checked) override;
  [[nodiscard]] std::optional<std::string> Text(std::string_view question, std::string_view default_text,
                                                const Validator& validate) override;

private:
  bool Drive(State& state);

  std::function<std::optional<KeyEvent>()> next_key_;
  std::ostream& output_;
};

/// Asks with numbered choices, one answer per input line; used where a raw terminal is unavailable.
class LinePrompter final : public Prompter {
public:
  LinePrompter(std::istream& input, std::ostream& output);

  [[nodiscard]] std::optional<std::size_t> Select(std::string_view question, std::span<const Choice> choices,
                                                  std::size_t initial) override;
  [[nodiscard]] std::optional<std::vector<std::size_t>>
  MultiSelect(std::string_view question, std::span<const Choice> choices,
              std::span<const std::size_t> initially_checked) override;
  [[nodiscard]] std::optional<std::string> Text(std::string_view question, std::string_view default_text,
                                                const Validator& validate) override;

private:
  void PrintChoices(std::span<const Choice> choices);

  std::istream& input_;
  std::ostream& output_;
};

/// A prompter for the process's standard streams, or none when standard input or output is not a terminal.
/// It uses raw key input where the terminal supports it and numbered line input otherwise.
[[nodiscard]] std::unique_ptr<Prompter> MakeTerminalPrompter();

} // namespace huxerui::cli::prompt
