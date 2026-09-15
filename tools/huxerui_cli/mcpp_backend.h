#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "process_runner.h"

namespace huxerui::cli {

class PlatformDriver;

/// What one CLI platform means to mcpp: the target row it builds and runs, and
/// the `mcpp pack` that produces the thing a user installs there.
struct McppTarget {
  /// The canonical target triple `build` and `run` take, e.g. `x86_64-linux-android`.
  std::string triple;
  /// The format `mcpp run --format` packages and hands the runner, e.g. `apk`; empty when
  /// `mcpp run` executes the program itself.
  std::string run_format;
  /// The distribution format `mcpp pack --format` produces: the kind of artifact `huxerui
  /// package` produces for the same platform under CMake, e.g. `setup` on Windows.
  std::string pack_format;
  /// The target rows `mcpp pack` combines into that one artifact. The Android APK carries
  /// every ABI the Gradle template builds; every other platform packs its own row.
  std::vector<std::string> pack_triples;
  /// The root-package features `mcpp pack` names. `windows-installer` on Windows: it builds the
  /// Setup.exe's installer interface, which CMake also builds for a package build only.
  std::vector<std::string> pack_features;
};

/// The architecture segment of the host's own triple.
[[nodiscard]] std::string_view McppHostArchitecture() noexcept;

/// Maps a CLI platform identifier to its mcpp target.
/// @param platform_id One of the CLI's platform identifiers.
/// @param physical_device Whether the selected device is physical hardware; selects the device row over the
///        emulator or simulator row for Android and iOS.
/// @param host_architecture The host's architecture segment, which the desktop rows take.
/// @return The target, or a target with an empty triple for an unknown platform.
[[nodiscard]] McppTarget ResolveMcppTarget(std::string_view platform_id, bool physical_device,
                                           std::string_view host_architecture);

/// Reads the platforms an mcpp package declares in `[package] platforms`, as CLI platform identifiers
/// (`emscripten` is `web`). A manifest without the key gets the three desktop platforms.
[[nodiscard]] std::vector<std::string> McppProjectPlatforms(const std::filesystem::path& manifest);

/// Renders `[package] platforms` for a set of CLI platforms, in mcpp's own vocabulary.
[[nodiscard]] std::string McppPlatformsLine(std::span<const std::string> platform_ids);

/// `mcpp build --target <triple> --profile <dev|release>`.
[[nodiscard]] ProcessCommand McppBuildCommand(const std::filesystem::path& root, const McppTarget& target, bool release);
/// `mcpp run --target <triple> [--format <run format>] --profile <dev|release>`.
[[nodiscard]] ProcessCommand McppRunCommand(const std::filesystem::path& root, const McppTarget& target, bool release);

/// The environment a selected device reaches the row's runner through: `ANDROID_SERIAL` for `adb-run`,
/// `SIMCTL_RUN_UDID` for `simctl-run`, `DEVICECTL_RUN_DEVICE` for `devicectl-run` on the iOS device row;
/// empty for a row without a device or when no device was selected.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> McppRunEnvironment(const McppTarget& target,
                                                                                   std::string_view device_id);
/// `mcpp pack --target <triple>... --format <pack format> [--features <pack features>] --profile <dev|release>`.
[[nodiscard]] ProcessCommand McppPackCommand(const std::filesystem::path& root, const McppTarget& target, bool release);
/// The artifacts `mcpp pack` reports on its `Packed <path>` lines, resolved against the package root
/// (a path mcpp shortens to `~/...` is resolved against `home`). A multi-row pack's `Packed leg` lines
/// name intermediate rows and are not artifacts.
[[nodiscard]] std::vector<std::filesystem::path> McppPackedArtifacts(std::string_view output,
                                                                     const std::filesystem::path& root,
                                                                     const std::filesystem::path& home);
/// The directory `mcpp pack --format web` writes.
[[nodiscard]] std::filesystem::path McppWebOutputDirectory(const std::filesystem::path& root);

} // namespace huxerui::cli
