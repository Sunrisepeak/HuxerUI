module;

#ifdef __EMSCRIPTEN__
#define CTS_PROBE_CONSOLE 1
#else
#define CTS_PROBE_CONSOLE 0
#endif

export module cts.probe;

import std;

export namespace cts {

/// Appends observed runtime facts to the file named by HUXERUI_CTS_PROBE so the conformance runner can read what a
/// started application resolved. Recording is inert when the variable is absent. A Web page has no environment or
/// file system the runner can read, so there each fact is a console line the runner reads from the browser.
class Probe {
public:
  void Record(std::string_view key, const std::string& value) const {
    if constexpr (CTS_PROBE_CONSOLE) {
      std::cout << "HUXERUI_CTS_PROBE " << key << '=' << value << std::endl;
      return;
    }
    const char* path = std::getenv("HUXERUI_CTS_PROBE");
    if (path == nullptr || *path == '\0') return;
    std::ofstream output(path, std::ios::app);
    output << key << '=' << value << '\n';
  }
};

} // namespace cts
