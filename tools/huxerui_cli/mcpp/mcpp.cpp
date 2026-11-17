#include "mcpp.h"

#include <stdexcept>
#include <utility>
#include <vector>

namespace huxerui::cli::mcpp {
namespace {

std::vector<std::string> CommonArguments(std::string_view verb, const CommandOptions& options) {
  std::vector<std::string> arguments{std::string(verb)};
  const std::string_view triple = TargetTriple(options.platform);
  if (!triple.empty()) {
    arguments.insert(arguments.end(), {"--target", std::string(triple)});
  }
  // mcpp's development profile is the contract's debug profile.
  if (options.profile == "release") {
    arguments.emplace_back("--release");
  }
  return arguments;
}

} // namespace

std::string_view TargetTriple(std::string_view platform) {
  if (platform == "linux" || platform == "windows" || platform == "macos") {
    return {};
  }
  if (platform == "web") {
    return "wasm32-emscripten";
  }
  // The application core the platform shells consume: arm64 Android, and the iOS Simulator on Apple silicon.
  if (platform == "android") {
    return "aarch64-linux-android";
  }
  if (platform == "ios") {
    return "aarch64-ios-sim";
  }
  throw std::invalid_argument("the mcpp integration does not build platform " + std::string(platform));
}

ProcessCommand BuildCommand(const CommandOptions& options) {
  return {"mcpp", CommonArguments("build", options), options.project_root};
}

ProcessCommand RunCommand(const CommandOptions& options) {
  return {"mcpp", CommonArguments("run", options), options.project_root};
}

} // namespace huxerui::cli::mcpp
