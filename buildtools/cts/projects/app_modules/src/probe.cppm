export module cts.probe;

import std;

export namespace cts {

/// Appends observed runtime facts to the file named by HUXERUI_CTS_PROBE so the conformance runner can read what a
/// started application resolved. Recording is inert when the variable is absent.
class Probe {
public:
  void Record(std::string_view key, const std::string& value) const {
    const char* path = std::getenv("HUXERUI_CTS_PROBE");
    if (path == nullptr || *path == '\0') return;
    std::ofstream output(path, std::ios::app);
    output << key << '=' << value << '\n';
  }
};

} // namespace cts
