#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "process_runner.h"

namespace huxerui::cli::mcpp {

/// What the CLI asks the mcpp integration to build or run.
struct CommandOptions {
  /// Root directory containing `mcpp.toml`.
  std::filesystem::path project_root;
  /// HuxerUI platform identifier, such as `linux` or `web`.
  std::string platform;
  /// Logical profile, `debug` or `release`.
  std::string profile = "debug";
};

/// The mcpp target triple a platform is built for, or an empty string for the host's own desktop platform.
/// @throws std::invalid_argument for a platform the mcpp integration does not build.
[[nodiscard]] std::string_view TargetTriple(std::string_view platform);

/// The `mcpp build` command for a platform and profile.
[[nodiscard]] ProcessCommand BuildCommand(const CommandOptions& options);

/// The `mcpp run` command, which builds and starts the application on the host.
[[nodiscard]] ProcessCommand RunCommand(const CommandOptions& options);

} // namespace huxerui::cli::mcpp
