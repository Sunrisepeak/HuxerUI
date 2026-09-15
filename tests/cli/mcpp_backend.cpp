#include "support.h"

#include "mcpp_backend.h"
#include "process_runner.h"

#include <fstream>

using namespace huxerui::cli::test;
using huxerui::cli::McppBuildCommand;
using huxerui::cli::McppPackCommand;
using huxerui::cli::McppPlatformsLine;
using huxerui::cli::McppProjectPlatforms;
using huxerui::cli::McppRunCommand;
using huxerui::cli::McppRunEnvironment;
using huxerui::cli::ResolveMcppTarget;

namespace {

void Write(const std::filesystem::path& path, std::string_view content) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << content;
}

} // namespace

TEST_CASE("HuxerUICliMapsPlatformsToMcppTargets") {
  using Triples = std::vector<std::string>;
  // The desktop rows follow the host; the mobile rows default to what mcpp can
  // run without a signature and switch to the device row for hardware.
  REQUIRE(ResolveMcppTarget("linux", false, "x86_64").triple == "x86_64-linux-gnu");
  REQUIRE(ResolveMcppTarget("windows", false, "x86_64").triple == "x86_64-windows-msvc");
  REQUIRE(ResolveMcppTarget("macos", false, "aarch64").triple == "aarch64-macos");
  REQUIRE(ResolveMcppTarget("web", false, "x86_64").triple == "wasm32-emscripten");
  REQUIRE(ResolveMcppTarget("android", false, "x86_64").triple == "x86_64-linux-android");
  REQUIRE(ResolveMcppTarget("android", true, "x86_64").triple == "aarch64-linux-android");
  REQUIRE(ResolveMcppTarget("ios", false, "aarch64").triple == "aarch64-ios-sim");
  REQUIRE(ResolveMcppTarget("ios", true, "aarch64").triple == "aarch64-ios");
  REQUIRE(ResolveMcppTarget("plan9", false, "x86_64").triple.empty());

  // What `run` hands a runner: the bundle on macOS and iOS, the APK on Android,
  // the program itself on the other rows.
  REQUIRE(ResolveMcppTarget("linux", false, "x86_64").run_format.empty());
  REQUIRE(ResolveMcppTarget("windows", false, "x86_64").run_format.empty());
  REQUIRE(ResolveMcppTarget("macos", false, "aarch64").run_format == "app");
  REQUIRE(ResolveMcppTarget("web", false, "x86_64").run_format.empty());
  REQUIRE(ResolveMcppTarget("android", false, "x86_64").run_format == "apk");
  REQUIRE(ResolveMcppTarget("ios", false, "aarch64").run_format == "app");

  // What `package` produces is what CMake's `huxerui package` produces there.
  REQUIRE(ResolveMcppTarget("linux", false, "x86_64").pack_format == "appimage");
  REQUIRE(ResolveMcppTarget("windows", false, "x86_64").pack_format == "setup");
  REQUIRE(ResolveMcppTarget("macos", false, "aarch64").pack_format == "dmg");
  REQUIRE(ResolveMcppTarget("web", false, "x86_64").pack_format == "web");
  REQUIRE(ResolveMcppTarget("android", false, "x86_64").pack_format == "apk");
  REQUIRE(ResolveMcppTarget("ios", false, "aarch64").pack_format == "app");
  // One APK for the ABIs the Gradle template lists, whichever device selected the row.
  REQUIRE(ResolveMcppTarget("android", false, "x86_64").pack_triples == Triples{"aarch64-linux-android", "x86_64-linux-android"});
  REQUIRE(ResolveMcppTarget("android", true, "x86_64").pack_triples == Triples{"aarch64-linux-android", "x86_64-linux-android"});
  REQUIRE(ResolveMcppTarget("macos", false, "aarch64").pack_triples == Triples{"aarch64-macos"});
}

TEST_CASE("HuxerUICliBuildsMcppCommandsFromATarget") {
  const auto android = ResolveMcppTarget("android", false, "x86_64");
  const auto build = McppBuildCommand("/p", android, false);
  REQUIRE(build.executable == "mcpp");
  REQUIRE(build.arguments == std::vector<std::string>{"build", "--target", "x86_64-linux-android", "--profile", "dev"});
  REQUIRE(build.working_directory == std::filesystem::path("/p"));
  REQUIRE(McppBuildCommand("/p", android, true).arguments.back() == "release");
  // An APK is what the runner installs, so the run packages first.
  REQUIRE(McppRunCommand("/p", android, false).arguments ==
          std::vector<std::string>{"run", "--target", "x86_64-linux-android", "--format", "apk", "--profile", "dev"});
  // A debug package names its profile: `mcpp pack` alone is a release pack, and it has no `--release`.
  REQUIRE(McppPackCommand("/p", android, false).arguments ==
          std::vector<std::string>{"pack", "--target", "aarch64-linux-android", "--target", "x86_64-linux-android",
                                   "--format", "apk", "--profile", "dev"});
  const auto linux = ResolveMcppTarget("linux", false, "x86_64");
  REQUIRE(McppRunCommand("/p", linux, false).arguments ==
          std::vector<std::string>{"run", "--target", "x86_64-linux-gnu", "--profile", "dev"});
  REQUIRE(McppPackCommand("/p", linux, true).arguments ==
          std::vector<std::string>{"pack", "--target", "x86_64-linux-gnu", "--format", "appimage", "--profile", "release"});
  const auto macos = ResolveMcppTarget("macos", false, "aarch64");
  REQUIRE(McppRunCommand("/p", macos, false).arguments ==
          std::vector<std::string>{"run", "--target", "aarch64-macos", "--format", "app", "--profile", "dev"});
  REQUIRE(McppPackCommand("/p", macos, false).arguments ==
          std::vector<std::string>{"pack", "--target", "aarch64-macos", "--format", "dmg", "--profile", "dev"});
  // The Setup.exe's installer interface is built only for a pack, as CMake builds it for a package build.
  REQUIRE(McppPackCommand("/p", ResolveMcppTarget("windows", false, "x86_64"), false).arguments ==
          std::vector<std::string>{"pack", "--target", "x86_64-windows-msvc", "--format", "setup",
                                   "--features", "windows-installer", "--profile", "dev"});
}

TEST_CASE("HuxerUICliReadsTheArtifactsAnMcppPackReports") {
  const std::filesystem::path root = std::filesystem::current_path() / "p";
  const std::filesystem::path home = std::filesystem::current_path() / "home";
  const std::filesystem::path elsewhere = std::filesystem::current_path() / "abs" / "app.AppImage";
  const std::string output = "   Compiling app v0.1.0 (.)\n"
                             "  Packed leg x86_64-linux-android  [x86_64]\n"
                             "      Packed target/.build-mcpp/out/app.apk\r\n"
                             "      Packed ~/elsewhere/app.dmg\n"
                             "      Packed " + elsewhere.generic_string() + "\n"
                             "    Finished release\n";
  const auto artifacts = huxerui::cli::McppPackedArtifacts(output, root, home);
  REQUIRE(artifacts == std::vector<std::filesystem::path>{
                           (root / "target/.build-mcpp/out/app.apk").lexically_normal(),
                           (home / "elsewhere/app.dmg").lexically_normal(),
                           elsewhere.lexically_normal()});
  REQUIRE(huxerui::cli::McppPackedArtifacts("nothing packed\n", root, home).empty());
}

TEST_CASE("HuxerUICliHandsTheSelectedDeviceToTheRunner") {
  using Environment = std::vector<std::pair<std::string, std::string>>;
  const auto android = ResolveMcppTarget("android", false, "x86_64");
  REQUIRE(McppRunEnvironment(android, "emulator-5554") == Environment{{"ANDROID_SERIAL", "emulator-5554"}});
  const auto ios = ResolveMcppTarget("ios", false, "aarch64");
  REQUIRE(McppRunEnvironment(ios, "1234-ABCD") == Environment{{"SIMCTL_RUN_UDID", "1234-ABCD"}});
  // A physical iOS device selects the device row, whose runner is devicectl-run.
  const auto device = ResolveMcppTarget("ios", true, "aarch64");
  REQUIRE(device.triple == "aarch64-ios");
  REQUIRE(McppRunEnvironment(device, "00008110-ABCD") == Environment{{"DEVICECTL_RUN_DEVICE", "00008110-ABCD"}});
  // No device selected: nothing is set, so the runner's own default applies.
  REQUIRE(McppRunEnvironment(android, "").empty());
  // A desktop row has no device to select.
  REQUIRE(McppRunEnvironment(ResolveMcppTarget("linux", false, "x86_64"), "ignored").empty());
}

TEST_CASE("HuxerUICliReadsAnMcppProjectsPlatforms") {
  TemporaryDirectory temporary;
  const std::filesystem::path manifest = temporary.Path() / "mcpp.toml";
  Write(manifest, "[package]\nname = \"sample\"\nplatforms = [\"linux\", \"android\", \"emscripten\", \"plan9\"]\n");
  // mcpp says `emscripten` where the CLI says `web`; an unknown name is dropped.
  REQUIRE(McppProjectPlatforms(manifest) == std::vector<std::string>{"linux", "android", "web"});
  Write(manifest, "[package]\nname = \"sample\"\n");
  REQUIRE(McppProjectPlatforms(manifest) == std::vector<std::string>{"linux", "windows", "macos"});
  REQUIRE(McppPlatformsLine(std::vector<std::string>{"web", "ios"}) == "platforms = [\"emscripten\", \"ios\"]");
}

TEST_CASE("HuxerUICliDiscoversAnMcppProject") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "sample";
  std::filesystem::create_directories(root / "src");
  Write(root / "mcpp.toml", "[package]\nname = \"sample\"\nplatforms = [\"linux\", \"web\"]\n");
  Write(root / "build.mcpp", "int main() { return 0; }\n");
  const huxerui::cli::Project project = huxerui::cli::DiscoverProject(root / "src");
  REQUIRE(project.build_system == huxerui::cli::BuildSystem::Mcpp);
  REQUIRE(project.root == root);
  REQUIRE(project.platforms == std::vector<std::string>{"linux", "web"});
}

TEST_CASE("HuxerUICliDoctorLeavesAnMcppProjectsToolchainsToMcpp") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "sample";
  std::filesystem::create_directories(root);
  Write(root / "mcpp.toml", "[package]\nname = \"sample\"\nplatforms = [\"linux\", \"web\"]\n");
  Write(root / "build.mcpp", "int main() { return 0; }\n");

  // Read-only: the tool the verbs need and a pointer to mcpp's own diagnosis,
  // which resolves the build and so is not run; none of the CMake path's rows.
  const Invocation doctor = Invoke(root, {"doctor"});
  REQUIRE(doctor.result == (huxerui::cli::FindExecutable("mcpp") ? 0 : 1));
  REQUIRE(doctor.output.find("Build system: mcpp\n") != std::string::npos);
  REQUIRE(doctor.output.find("] mcpp") != std::string::npos);
  REQUIRE(doctor.output.find("`mcpp self doctor`") != std::string::npos);
  REQUIRE(doctor.output.find("> mcpp") == std::string::npos);
  REQUIRE(doctor.output.find("] cmake") == std::string::npos);
  REQUIRE(doctor.output.find("HUXERUI_HOME") == std::string::npos);

  const Invocation android = Invoke(root, {"doctor", "android"});
  REQUIRE(android.result == 1);
  REQUIRE(android.output.find("[error] platform is not enabled by this project: android") != std::string::npos);
}

#if !defined(_WIN32)
TEST_CASE("HuxerUICliPublishesAnMcppPackageWhereCMakePublishesOne") {
  TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.Path() / "sample";
  const std::filesystem::path bin = temporary.Path() / "bin";
  std::filesystem::create_directories(root);
  std::filesystem::create_directories(bin);
  Write(root / "mcpp.toml", "[package]\nname = \"sample\"\nplatforms = [\"linux\"]\n");
  Write(root / "build.mcpp", "int main() { return 0; }\n");
  // A stand-in for `mcpp pack`: it writes the artifact where a dist member would and reports it the way mcpp does.
  const std::filesystem::path fake = bin / "mcpp";
  Write(fake, "#!/bin/sh\n"
              "mkdir -p target/.build-mcpp/out\n"
              "printf 'appimage' > target/.build-mcpp/out/sample-x86_64.AppImage\n"
              "echo '   Compiling sample v0.1.0 (.)'\n"
              "echo '      Packed target/.build-mcpp/out/sample-x86_64.AppImage'\n");
  std::filesystem::permissions(fake, std::filesystem::perms::owner_all);
  const std::string path = huxerui::cli::ReadEnvironmentVariable("PATH").value_or("");
  huxerui::cli::SetProcessEnvironmentVariable("PATH", bin.string() + ":" + path);
  const Invocation package = Invoke(root, {"package", "linux"});
  huxerui::cli::SetProcessEnvironmentVariable("PATH", path);

  REQUIRE(package.result == 0);
  REQUIRE(package.output.find("Packed target/.build-mcpp/out/sample-x86_64.AppImage") != std::string::npos);
  REQUIRE(Read(root / "dist" / "linux" / "sample-x86_64.AppImage") == "appimage");
}
#endif

TEST_CASE("HuxerUICliCreatesAnMcppProjectForSelectedPlatforms") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(),
      {"create", "app", "Sample-App", "--build", "mcpp", "--platform", "android,web", "--agent", "none"});
  REQUIRE(invocation.result == 0);
  const std::string manifest = Read(temporary.Path() / "Sample-App" / "mcpp.toml");
  REQUIRE(manifest.find("\nplatforms = [\"android\", \"emscripten\"]\n") != std::string::npos);
}
