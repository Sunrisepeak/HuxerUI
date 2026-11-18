#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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

/// The mcpp package format a platform's distributable is written in.
/// @param platform HuxerUI platform identifier.
/// @param requested The `--format` the caller selected, or empty for the platform's default.
/// @return The format: `appimage`, `deb`, or `rpm` on Linux, `setup` on Windows, `dmg` on macOS, `web` on the Web,
///         `apk` on Android, and `app` on iOS.
/// @throws std::runtime_error for a platform mcpp has no package format for.
[[nodiscard]] std::string PackageFormat(std::string_view platform, std::string_view requested);

/// The `mcpp pack` command that writes a platform's distributable and reports it as JSON on standard output.
[[nodiscard]] ProcessCommand PackCommand(const CommandOptions& options, std::string_view format);

/// One distributable `mcpp pack` reported.
struct PackedArtifact {
  /// Absolute path of the file or directory.
  std::filesystem::path path;
  /// `file` or `dir`.
  std::string type;
};

/// Reads the artifacts from the `mcpp.pack` envelope `mcpp pack --message-format json` writes.
/// @throws std::runtime_error when the envelope reports no artifact.
[[nodiscard]] std::vector<PackedArtifact> ParsePackArtifacts(std::string_view envelope);

} // namespace huxerui::cli::mcpp
