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

/// The string value that follows `"key":` at or after `from`, with JSON escapes resolved, and where it ended.
std::pair<std::string, std::size_t> JsonStringAfter(std::string_view json, std::string_view key, std::size_t from,
                                                    std::size_t limit) {
  const std::string marker = "\"" + std::string(key) + "\"";
  std::size_t at = json.find(marker, from);
  if (at == std::string_view::npos || at >= limit) {
    return {{}, std::string_view::npos};
  }
  at = json.find(':', at + marker.size());
  at = at == std::string_view::npos ? at : json.find('"', at + 1);
  if (at == std::string_view::npos || at >= limit) {
    return {{}, std::string_view::npos};
  }
  std::string value;
  for (std::size_t index = at + 1; index < json.size(); ++index) {
    const char character = json[index];
    if (character == '"') {
      return {value, index + 1};
    }
    if (character == '\\' && index + 1 < json.size()) {
      const char escaped = json[++index];
      value += escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped;
      continue;
    }
    value += character;
  }
  return {{}, std::string_view::npos};
}

/// The position of the bracket that closes the one at `open`, skipping strings, or npos.
std::size_t MatchingBracket(std::string_view json, std::size_t open) {
  int depth = 0;
  bool in_string = false;
  for (std::size_t index = open; index < json.size(); ++index) {
    const char character = json[index];
    if (in_string) {
      if (character == '\\') {
        ++index;
      } else if (character == '"') {
        in_string = false;
      }
    } else if (character == '"') {
      in_string = true;
    } else if (character == '[' || character == '{') {
      ++depth;
    } else if ((character == ']' || character == '}') && --depth == 0) {
      return index;
    }
  }
  return std::string_view::npos;
}

} // namespace

std::string PackageFormat(std::string_view platform, std::string_view requested) {
  if (platform == "linux") {
    return requested.empty() ? "appimage" : std::string(requested);
  }
  if (platform == "windows") {
    return "msi";
  }
  if (platform == "macos") {
    return "dmg";
  }
  if (platform == "web") {
    return "web";
  }
  throw std::runtime_error("packaging an mcpp project for " + std::string(platform) +
                           " is not available yet; no other format is produced in its place");
}

ProcessCommand PackCommand(const CommandOptions& options, std::string_view format) {
  std::vector<std::string> arguments{"pack", "--pkg-format", std::string(format), "--message-format", "json"};
  const std::string_view triple = TargetTriple(options.platform);
  if (!triple.empty()) {
    arguments.insert(arguments.end(), {"--target", std::string(triple)});
  }
  // mcpp packs the release profile by default, which is the contract's default for package.
  if (options.profile == "debug") {
    arguments.emplace_back("--dev");
  }
  return {"mcpp", std::move(arguments), options.project_root};
}

std::vector<PackedArtifact> ParsePackArtifacts(std::string_view envelope) {
  if (envelope.find("\"mcpp.pack\"") == std::string_view::npos) {
    throw std::runtime_error("mcpp pack wrote no mcpp.pack result");
  }
  const std::size_t list = envelope.find("\"artifacts\"");
  const std::size_t open = list == std::string_view::npos ? list : envelope.find('[', list);
  const std::size_t close = open == std::string_view::npos ? open : MatchingBracket(envelope, open);
  std::vector<PackedArtifact> artifacts;
  for (std::size_t at = open; at != std::string_view::npos && close != std::string_view::npos && at < close;) {
    const std::size_t object = envelope.find('{', at);
    if (object == std::string_view::npos || object > close) {
      break;
    }
    const std::size_t end = MatchingBracket(envelope, object);
    const std::string path = JsonStringAfter(envelope, "path", object, end).first;
    const std::string type = JsonStringAfter(envelope, "type", object, end).first;
    if (!path.empty()) {
      artifacts.push_back({std::filesystem::path(path), type.empty() ? std::string("file") : type});
    }
    at = end == std::string_view::npos ? end : end + 1;
  }
  if (artifacts.empty()) {
    throw std::runtime_error("mcpp pack reported no artifact");
  }
  return artifacts;
}

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
