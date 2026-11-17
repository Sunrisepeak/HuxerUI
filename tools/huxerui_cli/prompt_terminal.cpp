#include "prompt.h"

#include <iostream>
#include <memory>
#include <optional>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace huxerui::cli::prompt {
namespace {

#if defined(_WIN32)

bool StandardStreamsAreTerminals() {
  return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
}

// Raw key input through the console's virtual-terminal input mode, which reports arrow keys as the same escape
// sequences a POSIX terminal sends. The original console modes are restored when the prompter is destroyed.
class RawTerminal final {
public:
  RawTerminal() {
    input_ = GetStdHandle(STD_INPUT_HANDLE);
    output_ = GetStdHandle(STD_OUTPUT_HANDLE);
    if (input_ == INVALID_HANDLE_VALUE || output_ == INVALID_HANDLE_VALUE || !GetConsoleMode(input_, &input_mode_) ||
        !GetConsoleMode(output_, &output_mode_)) {
      return;
    }
    const DWORD raw_input =
        (input_mode_ & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT)) | ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (!SetConsoleMode(output_, output_mode_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
      return;
    }
    if (!SetConsoleMode(input_, raw_input)) {
      SetConsoleMode(output_, output_mode_);
      return;
    }
    active_ = true;
  }

  ~RawTerminal() {
    if (active_) {
      SetConsoleMode(input_, input_mode_);
      SetConsoleMode(output_, output_mode_);
    }
  }

  RawTerminal(const RawTerminal&) = delete;
  RawTerminal& operator=(const RawTerminal&) = delete;

  [[nodiscard]] bool Active() const noexcept {
    return active_;
  }

  [[nodiscard]] std::optional<unsigned char> ReadByte() {
    unsigned char byte = 0;
    DWORD read = 0;
    if (!ReadFile(input_, &byte, 1, &read, nullptr) || read != 1) {
      return std::nullopt;
    }
    return byte;
  }

private:
  HANDLE input_ = INVALID_HANDLE_VALUE;
  HANDLE output_ = INVALID_HANDLE_VALUE;
  DWORD input_mode_ = 0;
  DWORD output_mode_ = 0;
  bool active_ = false;
};

#else

bool StandardStreamsAreTerminals() {
  return isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0;
}

// Raw key input without echo or signal generation; Ctrl-C arrives as a byte so the prompt can cancel and the
// terminal settings are restored when the prompter is destroyed.
class RawTerminal final {
public:
  RawTerminal() {
    if (tcgetattr(STDIN_FILENO, &original_) != 0) {
      return;
    }
    termios raw = original_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    active_ = tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0;
  }

  ~RawTerminal() {
    if (active_) {
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
    }
  }

  RawTerminal(const RawTerminal&) = delete;
  RawTerminal& operator=(const RawTerminal&) = delete;

  [[nodiscard]] bool Active() const noexcept {
    return active_;
  }

  [[nodiscard]] std::optional<unsigned char> ReadByte() {
    unsigned char byte = 0;
    if (read(STDIN_FILENO, &byte, 1) != 1) {
      return std::nullopt;
    }
    return byte;
  }

private:
  termios original_{};
  bool active_ = false;
};

#endif

class TerminalPrompter final : public Prompter {
public:
  TerminalPrompter() = default;

  [[nodiscard]] std::optional<std::size_t> Select(std::string_view question, std::span<const Choice> choices,
                                                  std::size_t initial) override {
    Session session;
    if (!session.terminal.Active()) {
      return LinePrompter(std::cin, std::cout).Select(question, choices, initial);
    }
    return session.prompter.Select(question, choices, initial);
  }

  [[nodiscard]] std::optional<std::vector<std::size_t>>
  MultiSelect(std::string_view question, std::span<const Choice> choices,
              std::span<const std::size_t> initially_checked) override {
    Session session;
    if (!session.terminal.Active()) {
      return LinePrompter(std::cin, std::cout).MultiSelect(question, choices, initially_checked);
    }
    return session.prompter.MultiSelect(question, choices, initially_checked);
  }

  [[nodiscard]] std::optional<std::string> Text(std::string_view question, std::string_view default_text,
                                                const Validator& validate) override {
    Session session;
    if (!session.terminal.Active()) {
      return LinePrompter(std::cin, std::cout).Text(question, default_text, validate);
    }
    return session.prompter.Text(question, default_text, validate);
  }

private:
  // Raw mode lasts for one question so that output between questions, and any failure, sees a normal terminal.
  struct Session {
    RawTerminal terminal;
    KeyDecoder decoder;
    KeyPrompter prompter{
        [this]() -> std::optional<KeyEvent> {
          while (const std::optional<unsigned char> byte = terminal.ReadByte()) {
            if (std::optional<KeyEvent> event = decoder.Feed(*byte)) {
              return event;
            }
          }
          return std::nullopt;
        },
        std::cout,
    };
  };
};

} // namespace

std::unique_ptr<Prompter> MakeTerminalPrompter() {
  if (!StandardStreamsAreTerminals()) {
    return nullptr;
  }
  return std::make_unique<TerminalPrompter>();
}

} // namespace huxerui::cli::prompt
