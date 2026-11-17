#include "prompt.h"

#include <algorithm>
#include <charconv>
#include <istream>
#include <ostream>
#include <utility>

namespace huxerui::cli::prompt {
namespace {

constexpr std::string_view select_help = "  Use up and down to move, enter to confirm.";
constexpr std::string_view multi_select_help = "  Use up and down to move, space to select, enter to confirm.";

std::string ChoiceLine(const Choice& choice) {
  return choice.hint.empty() ? choice.label : choice.label + "  (" + choice.hint + ')';
}

void PopCodePoint(std::string& text) {
  // UTF-8 continuation bytes have the form 10xxxxxx; remove them with their lead byte.
  while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0U) == 0x80U) {
    text.pop_back();
  }
  if (!text.empty()) {
    text.pop_back();
  }
}

std::string Trim(std::string_view value) {
  const std::size_t first = value.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    return {};
  }
  const std::size_t last = value.find_last_not_of(" \t\r");
  return std::string(value.substr(first, last - first + 1));
}

std::optional<std::size_t> ParseChoiceNumber(std::string_view value, std::size_t count) {
  std::size_t number = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
  if (error != std::errc{} || end != value.data() + value.size() || number == 0 || number > count) {
    return std::nullopt;
  }
  return number - 1;
}

} // namespace

State MakeSelect(std::string question, std::vector<Choice> choices, std::size_t initial) {
  State state;
  state.kind = Kind::Select;
  state.question = std::move(question);
  state.choices = std::move(choices);
  state.cursor = initial < state.choices.size() ? initial : 0;
  return state;
}

State MakeMultiSelect(std::string question, std::vector<Choice> choices,
                      std::span<const std::size_t> initially_checked) {
  State state;
  state.kind = Kind::MultiSelect;
  state.question = std::move(question);
  state.choices = std::move(choices);
  state.checked.assign(state.choices.size(), false);
  for (const std::size_t index : initially_checked) {
    if (index < state.checked.size()) {
      state.checked[index] = true;
    }
  }
  return state;
}

State MakeText(std::string question, std::string default_text, Validator validate) {
  State state;
  state.kind = Kind::Text;
  state.question = std::move(question);
  state.default_text = std::move(default_text);
  state.validate = std::move(validate);
  return state;
}

void Apply(State& state, const KeyEvent& event) {
  if (state.status != Status::Active) {
    return;
  }
  if (event.key == Key::Cancel) {
    state.status = Status::Cancelled;
    return;
  }
  state.error.clear();
  const std::size_t count = state.choices.size();
  switch (state.kind) {
  case Kind::Select:
  case Kind::MultiSelect:
    if (event.key == Key::Up && count > 0) {
      state.cursor = (state.cursor + count - 1) % count;
    } else if (event.key == Key::Down && count > 0) {
      state.cursor = (state.cursor + 1) % count;
    } else if (event.key == Key::Space && state.kind == Kind::MultiSelect && count > 0) {
      state.checked[state.cursor] = !state.checked[state.cursor];
    } else if (event.key == Key::Enter) {
      if (state.kind == Kind::MultiSelect && std::ranges::none_of(state.checked, [](bool checked) { return checked; })) {
        state.error = "Select at least one option.";
      } else if (count > 0) {
        state.status = Status::Submitted;
      }
    }
    break;
  case Kind::Text:
    if (event.key == Key::Text) {
      state.text += event.text;
    } else if (event.key == Key::Space) {
      state.text += ' ';
    } else if (event.key == Key::Backspace) {
      PopCodePoint(state.text);
    } else if (event.key == Key::Enter) {
      std::string value = state.text.empty() ? state.default_text : state.text;
      const std::string problem = state.validate ? state.validate(value) : std::string{};
      if (!problem.empty()) {
        state.error = problem;
      } else {
        state.text = std::move(value);
        state.status = Status::Submitted;
      }
    }
    break;
  }
}

std::vector<std::string> Render(const State& state) {
  std::vector<std::string> lines;
  if (state.status != Status::Active) {
    lines.push_back("? " + state.question + ": " + (state.status == Status::Cancelled ? "cancelled" : Answer(state)));
    return lines;
  }
  if (state.kind == Kind::Text) {
    std::string line = "? " + state.question;
    if (!state.default_text.empty()) {
      line += " (" + state.default_text + ')';
    }
    lines.push_back(line + ": " + state.text);
  } else {
    lines.push_back("? " + state.question);
    for (std::size_t index = 0; index < state.choices.size(); ++index) {
      std::string line = index == state.cursor ? "  > " : "    ";
      if (state.kind == Kind::MultiSelect) {
        line += state.checked[index] ? "[x] " : "[ ] ";
      }
      lines.push_back(line + ChoiceLine(state.choices[index]));
    }
    lines.emplace_back(state.kind == Kind::MultiSelect ? multi_select_help : select_help);
  }
  if (!state.error.empty()) {
    lines.push_back("  ! " + state.error);
  }
  return lines;
}

std::string Answer(const State& state) {
  switch (state.kind) {
  case Kind::Select:
    return state.cursor < state.choices.size() ? state.choices[state.cursor].label : std::string{};
  case Kind::MultiSelect: {
    std::string answer;
    for (const std::size_t index : CheckedIndices(state)) {
      answer += (answer.empty() ? "" : ", ") + state.choices[index].label;
    }
    return answer;
  }
  case Kind::Text:
    return state.text;
  }
  return {};
}

std::vector<std::size_t> CheckedIndices(const State& state) {
  std::vector<std::size_t> indices;
  for (std::size_t index = 0; index < state.checked.size(); ++index) {
    if (state.checked[index]) {
      indices.push_back(index);
    }
  }
  return indices;
}

std::optional<KeyEvent> KeyDecoder::Feed(unsigned char byte) {
  if (!pending_.empty() && pending_.front() == '\x1b') {
    pending_.push_back(static_cast<char>(byte));
    if (pending_.size() == 2) {
      if (byte == '[' || byte == 'O') {
        return std::nullopt;
      }
      pending_.clear();
      return std::nullopt;
    }
    // CSI parameters are digits and semicolons; the final byte ends the sequence.
    if ((byte >= '0' && byte <= '9') || byte == ';') {
      return std::nullopt;
    }
    pending_.clear();
    if (byte == 'A') {
      return KeyEvent{Key::Up, {}};
    }
    if (byte == 'B') {
      return KeyEvent{Key::Down, {}};
    }
    return std::nullopt;
  }
  if (!pending_.empty()) {
    pending_.push_back(static_cast<char>(byte));
    const auto lead = static_cast<unsigned char>(pending_.front());
    const std::size_t length = lead >= 0xF0U ? 4 : lead >= 0xE0U ? 3 : 2;
    if ((byte & 0xC0U) != 0x80U) {
      pending_.clear();
      return std::nullopt;
    }
    if (pending_.size() < length) {
      return std::nullopt;
    }
    return KeyEvent{Key::Text, std::exchange(pending_, {})};
  }
  switch (byte) {
  case 0x03:
  case 0x04:
    return KeyEvent{Key::Cancel, {}};
  case '\r':
  case '\n':
    return KeyEvent{Key::Enter, {}};
  case 0x08:
  case 0x7F:
    return KeyEvent{Key::Backspace, {}};
  case ' ':
    return KeyEvent{Key::Space, {}};
  case 0x1B:
    pending_.assign(1, '\x1b');
    return std::nullopt;
  default:
    break;
  }
  if (byte >= 0xC0U) {
    pending_.assign(1, static_cast<char>(byte));
    return std::nullopt;
  }
  if (byte >= 0x20U && byte < 0x7FU) {
    return KeyEvent{Key::Text, std::string(1, static_cast<char>(byte))};
  }
  return std::nullopt;
}

KeyPrompter::KeyPrompter(std::function<std::optional<KeyEvent>()> next_key, std::ostream& output)
    : next_key_(std::move(next_key)), output_(output) {}

bool KeyPrompter::Drive(State& state) {
  std::size_t drawn = 0;
  const auto draw = [&] {
    if (drawn > 0) {
      // Move to the first line of the previous frame and clear everything below it.
      output_ << "\x1b[" << drawn << "F\x1b[J";
    }
    const std::vector<std::string> lines = Render(state);
    for (const std::string& line : lines) {
      output_ << line << '\n';
    }
    output_.flush();
    drawn = lines.size();
  };
  draw();
  while (state.status == Status::Active) {
    const std::optional<KeyEvent> event = next_key_();
    Apply(state, event.value_or(KeyEvent{Key::Cancel, {}}));
    draw();
  }
  return state.status == Status::Submitted;
}

std::optional<std::size_t> KeyPrompter::Select(std::string_view question, std::span<const Choice> choices,
                                               std::size_t initial) {
  State state = MakeSelect(std::string(question), {choices.begin(), choices.end()}, initial);
  if (!Drive(state)) {
    return std::nullopt;
  }
  return state.cursor;
}

std::optional<std::vector<std::size_t>> KeyPrompter::MultiSelect(std::string_view question,
                                                                 std::span<const Choice> choices,
                                                                 std::span<const std::size_t> initially_checked) {
  State state = MakeMultiSelect(std::string(question), {choices.begin(), choices.end()}, initially_checked);
  if (!Drive(state)) {
    return std::nullopt;
  }
  return CheckedIndices(state);
}

std::optional<std::string> KeyPrompter::Text(std::string_view question, std::string_view default_text,
                                             const Validator& validate) {
  State state = MakeText(std::string(question), std::string(default_text), validate);
  if (!Drive(state)) {
    return std::nullopt;
  }
  return state.text;
}

LinePrompter::LinePrompter(std::istream& input, std::ostream& output) : input_(input), output_(output) {}

void LinePrompter::PrintChoices(std::span<const Choice> choices) {
  for (std::size_t index = 0; index < choices.size(); ++index) {
    output_ << "  " << index + 1 << ") " << ChoiceLine(choices[index]) << '\n';
  }
}

std::optional<std::size_t> LinePrompter::Select(std::string_view question, std::span<const Choice> choices,
                                                std::size_t initial) {
  output_ << "? " << question << '\n';
  PrintChoices(choices);
  while (true) {
    output_ << "Choose 1-" << choices.size() << " [" << initial + 1 << "]: ";
    output_.flush();
    std::string line;
    if (!std::getline(input_, line)) {
      return std::nullopt;
    }
    const std::string answer = Trim(line);
    if (answer.empty()) {
      return initial;
    }
    if (const auto index = ParseChoiceNumber(answer, choices.size())) {
      return index;
    }
    output_ << "  ! Enter a number from 1 to " << choices.size() << ".\n";
  }
}

std::optional<std::vector<std::size_t>> LinePrompter::MultiSelect(std::string_view question,
                                                                  std::span<const Choice> choices,
                                                                  std::span<const std::size_t> initially_checked) {
  output_ << "? " << question << '\n';
  PrintChoices(choices);
  std::string defaults;
  for (const std::size_t index : initially_checked) {
    defaults += (defaults.empty() ? "" : ",") + std::to_string(index + 1);
  }
  while (true) {
    output_ << "Choose one or more, separated by commas [" << defaults << "]: ";
    output_.flush();
    std::string line;
    if (!std::getline(input_, line)) {
      return std::nullopt;
    }
    const std::string answer = Trim(line);
    if (answer.empty() && !initially_checked.empty()) {
      return std::vector<std::size_t>(initially_checked.begin(), initially_checked.end());
    }
    std::vector<bool> checked(choices.size(), false);
    bool valid = !answer.empty();
    std::size_t start = 0;
    while (valid && start <= answer.size()) {
      const std::size_t separator = answer.find(',', start);
      const std::size_t end = separator == std::string::npos ? answer.size() : separator;
      const auto index = ParseChoiceNumber(Trim(std::string_view(answer).substr(start, end - start)), choices.size());
      valid = index.has_value();
      if (valid) {
        checked[*index] = true;
      }
      if (separator == std::string::npos) {
        break;
      }
      start = separator + 1;
    }
    if (valid) {
      std::vector<std::size_t> indices;
      for (std::size_t index = 0; index < checked.size(); ++index) {
        if (checked[index]) {
          indices.push_back(index);
        }
      }
      return indices;
    }
    output_ << "  ! Enter numbers from 1 to " << choices.size() << ", separated by commas.\n";
  }
}

std::optional<std::string> LinePrompter::Text(std::string_view question, std::string_view default_text,
                                              const Validator& validate) {
  while (true) {
    output_ << "? " << question;
    if (!default_text.empty()) {
      output_ << " [" << default_text << ']';
    }
    output_ << ": ";
    output_.flush();
    std::string line;
    if (!std::getline(input_, line)) {
      return std::nullopt;
    }
    std::string answer = Trim(line);
    if (answer.empty()) {
      answer = std::string(default_text);
    }
    const std::string problem = validate ? validate(answer) : std::string{};
    if (problem.empty()) {
      return answer;
    }
    output_ << "  ! " << problem << '\n';
  }
}

} // namespace huxerui::cli::prompt
