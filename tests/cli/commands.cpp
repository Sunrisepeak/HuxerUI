#include "support.h"

#include "mcpp/mcpp.h"

using namespace huxerui::cli::test;

TEST_CASE("HuxerUICliHelpListsSupportedAgents") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"--help"});

  REQUIRE(invocation.result == 0);
  for (const std::string_view agent : {
           std::string_view{"codex"},
           std::string_view{"claude"},
           std::string_view{"antigravity"},
           std::string_view{"opencode"},
           std::string_view{"command-code"},
           std::string_view{"omp"},
           std::string_view{"dsh"},
           std::string_view{"zcode"},
       }) {
    REQUIRE(invocation.output.find(agent) != std::string::npos);
  }
  REQUIRE(invocation.output.find("all") != std::string::npos);
  REQUIRE(invocation.output.find("none") != std::string::npos);
  REQUIRE(invocation.output.find("--namespace <cpp-namespace>") != std::string::npos);
  REQUIRE(invocation.output.find("--target <public-cmake-target>") != std::string::npos);
  REQUIRE(invocation.output.find("--sdk <path>") != std::string::npos);
  REQUIRE(invocation.output.find("-d|--device <id>") != std::string::npos);
  REQUIRE(invocation.output.find("--java-home <path>") != std::string::npos);
  REQUIRE(invocation.output.find("--abi <abi-list>") != std::string::npos);
  REQUIRE(invocation.output.find("a common library's Preview enable all platforms") != std::string::npos);
  REQUIRE(invocation.output.find("[--build-system cmake|mcpp]") != std::string::npos);
  REQUIRE(invocation.output.find("huxerui mcpp") == std::string::npos);
}

TEST_CASE("HuxerUICliCreatesMcppDispatchCommands") {
  using huxerui::cli::mcpp::TargetTriple;
  REQUIRE(TargetTriple("linux").empty());
  REQUIRE(TargetTriple("windows").empty());
  REQUIRE(TargetTriple("web") == "wasm32-emscripten");
  REQUIRE(TargetTriple("android") == "aarch64-linux-android");
  REQUIRE(TargetTriple("ios") == "aarch64-ios-sim");
  REQUIRE_THROWS_AS(TargetTriple("plan9"), std::invalid_argument);

  const huxerui::cli::mcpp::CommandOptions web{"/project", "web", "release"};
  const huxerui::cli::ProcessCommand build = huxerui::cli::mcpp::BuildCommand(web);
  REQUIRE(build.executable == "mcpp");
  REQUIRE(build.arguments == std::vector<std::string>{"build", "--target", "wasm32-emscripten", "--release"});
  REQUIRE(build.working_directory == "/project");
  const huxerui::cli::ProcessCommand run = huxerui::cli::mcpp::RunCommand({"/project", "linux", "debug"});
  REQUIRE(run.arguments == std::vector<std::string>{"run"});
}

TEST_CASE("HuxerUICliCreatesMcppPackCommands") {
  using huxerui::cli::mcpp::PackageFormat;
  REQUIRE(PackageFormat("linux", "") == "appimage");
  REQUIRE(PackageFormat("linux", "rpm") == "rpm");
  REQUIRE(PackageFormat("windows", "") == "setup");
  REQUIRE(PackageFormat("macos", "") == "dmg");
  REQUIRE(PackageFormat("web", "") == "web");
  REQUIRE(PackageFormat("android", "") == "apk");
  REQUIRE(PackageFormat("ios", "") == "app");
  REQUIRE_THROWS_WITH(PackageFormat("plan9", ""), Catch::Matchers::ContainsSubstring("no package format"));

  const auto release = huxerui::cli::mcpp::PackCommand({"/project", "web", "release"}, "web");
  REQUIRE(release.arguments == std::vector<std::string>{"pack", "--pkg-format", "web", "--message-format", "json",
                                                        "--target", "wasm32-emscripten"});
  const auto debug = huxerui::cli::mcpp::PackCommand({"/project", "linux", "debug"}, "deb");
  REQUIRE(debug.arguments == std::vector<std::string>{"pack", "--pkg-format", "deb", "--message-format", "json",
                                                      "--dev"});
}

TEST_CASE("HuxerUICliReadsMcppPackResults") {
  const std::string envelope = R"({
 "data": {
  "artifacts": [
   {"format": "deb", "path": "/out/app_0.1.0_amd64.deb", "sha256": "00", "targets": ["x86_64-linux-gnu"],
    "type": "file"},
   {"format": "web", "path": "C:\\out\\web", "type": "dir"}
  ],
  "stage": {"dir": "/out/stage"}
 },
 "kind": "mcpp.pack"
})";
  const auto artifacts = huxerui::cli::mcpp::ParsePackArtifacts(envelope);
  REQUIRE(artifacts.size() == 2);
  REQUIRE(artifacts[0].path == std::filesystem::path("/out/app_0.1.0_amd64.deb"));
  REQUIRE(artifacts[0].type == "file");
  REQUIRE(artifacts[1].path == std::filesystem::path("C:\\out\\web"));
  REQUIRE(artifacts[1].type == "dir");
  REQUIRE_THROWS(huxerui::cli::mcpp::ParsePackArtifacts(R"({"kind": "mcpp.pack", "data": {"artifacts": []}})"));
  REQUIRE_THROWS(huxerui::cli::mcpp::ParsePackArtifacts("not json"));
}

TEST_CASE("HuxerUICliHasNoCommandNamedAfterABuildSystem") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"mcpp", "build"});
  REQUIRE(invocation.result == 2);
  REQUIRE(invocation.error.find("unknown command: mcpp") != std::string::npos);
}

TEST_CASE("HuxerUICliDispatchesToTheProjectsBuildSystem") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "mods", "--build-system", "mcpp", "--agent", "none"}).result == 0);
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "both", "--build-system", "cmake,mcpp", "--agent", "none"})
              .result == 0);
  const std::filesystem::path modules = temporary.Path() / "mods";
  const std::filesystem::path both = temporary.Path() / "both";

  // An option that would change the product is invalid usage for an integration that cannot honor it.
  for (const std::initializer_list<std::string_view> arguments : {
           std::initializer_list<std::string_view>{"build", "linux", "--abi", "x86_64"},
           std::initializer_list<std::string_view>{"build", "linux", "--lto", "on"},
           std::initializer_list<std::string_view>{"build", "linux", "--optimization", "size"},
           std::initializer_list<std::string_view>{"build", "linux", "--sdk", "."},
           std::initializer_list<std::string_view>{"run", "linux", "--watch"},
       }) {
    const Invocation invocation = Invoke(modules / "src", arguments);
    INFO(invocation.error);
    REQUIRE(invocation.result == 2);
    REQUIRE(invocation.error.find("mcpp project") != std::string::npos);
  }

  // A root declaring both build systems needs a selection, and the selection must name a declared one.
  const Invocation ambiguous = Invoke(both, {"build", "linux"});
  REQUIRE(ambiguous.result == 2);
  REQUIRE(ambiguous.error.find("select one with --build-system") != std::string::npos);
  REQUIRE(Invoke(both, {"build", "linux", "--build-system", "meson"}).result == 2);
  const Invocation undeclared = Invoke(modules, {"build", "linux", "--build-system", "cmake"});
  REQUIRE(undeclared.result == 2);
  REQUIRE(undeclared.error.find("no cmake declaration") != std::string::npos);

  // An option that only steers the CMake driver is reported and not applied; the command continues, here to
  // the platform check that stops it.
  const Invocation steered = Invoke(both, {"build", "plan9", "--build-system", "mcpp", "--generator", "Ninja"});
  REQUIRE(steered.result == 2);
  REQUIRE(steered.error.find("warning: --generator") != std::string::npos);
  REQUIRE(steered.error.find("unknown platform: plan9") != std::string::npos);

  // mcpp packages without the standard shell, so a named platform needs no shell directory; a platform the host
  // cannot build still stops the command, but a CMake project still needs the platform enabled.
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "linuxonly", "--build-system", "cmake,mcpp", "--platform",
                                    "linux", "--agent", "none"})
              .result == 0);
  const std::filesystem::path linux_only = temporary.Path() / "linuxonly";
  const std::string foreign{huxerui::cli::CurrentHostId() == "macos" ? "windows" : "ios"};
  for (const std::string_view command : {"build", "package"}) {
    const Invocation shellless = Invoke(linux_only, {command, foreign, "--build-system", "mcpp"});
    INFO(shellless.error);
    REQUIRE(shellless.result == 1);
    REQUIRE(shellless.error.find("cannot be built from host") != std::string::npos);
    const Invocation cmake =
        Invoke(linux_only, {command, foreign, "--build-system", "cmake", "--sdk", HUXERUI_TEST_SOURCE_DIRECTORY});
    INFO(cmake.error);
    REQUIRE(cmake.result == 1);
    REQUIRE(cmake.error.find("platform is not enabled by this project: " + foreign) != std::string::npos);
  }

  const Invocation open = Invoke(modules, {"open", "ios"});
  REQUIRE(open.result == 1);
  REQUIRE(open.error.find("CMake declaration") != std::string::npos);
}

TEST_CASE("HuxerUICliDiscoversTheNearestDeclaringRoot") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "outer", "--platform", "linux", "--agent", "none"}).result == 0);
  const std::filesystem::path outer = temporary.Path() / "outer";
  REQUIRE(Invoke(outer, {"create", "app", "inner", "--build-system", "mcpp", "--platform", "linux", "--agent", "none"})
              .result == 0);
  const huxerui::cli::Project project = huxerui::cli::DiscoverProject(outer / "inner/src");
  REQUIRE(project.root == outer / "inner");
  REQUIRE(project.build_systems == std::vector{huxerui::cli::BuildSystem::Mcpp});
  REQUIRE(huxerui::cli::DiscoverProject(outer / "src").root == outer);
}

TEST_CASE("HuxerUICliAddsPlatformsToMcppProjects") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "Mods", "--build-system", "mcpp", "--id", "dev.example.mods",
                                    "--platform", "linux", "--agent", "none"})
              .result == 0);
  const std::filesystem::path project = temporary.Path() / "Mods";
  const Invocation added = Invoke(project, {"platform", "add", "macos"});
  INFO(added.error);
  REQUIRE(added.result == 0);
  // The shell carries the identity recorded at creation and the modules form of the entry.
  REQUIRE(Read(project / "platform/macos/Info.plist.in").find("dev.example.mods") != std::string::npos);
  REQUIRE(Read(project / "platform/macos/main.cpp").find("import huxerui;") != std::string::npos);
}

TEST_CASE("HuxerUICliRejectsInvalidSdkAndDeviceOptions") {
  TemporaryDirectory temporary;
  for (const auto option : {"--sdk", "-d", "--generator", "--config", "--abi", "--java-home"}) {
    const Invocation missing = Invoke(temporary.Path(), {"build", "windows", option, "--help"});
    REQUIRE(missing.result == 2);
    REQUIRE(missing.error.find(std::string(option) + " requires a value") != std::string::npos);
  }

  const Invocation duplicate = Invoke(temporary.Path(), {"build", "windows", "--sdk", ".", "--sdk", "."});
  REQUIRE(duplicate.result == 2);
  REQUIRE(duplicate.error.find("--sdk may be specified only once") != std::string::npos);

  const Invocation duplicate_device = Invoke(temporary.Path(), {"r", "ios", "-d", "one", "--device", "two"});
  REQUIRE(duplicate_device.result == 2);
  REQUIRE(duplicate_device.error.find("--device may be specified only once") != std::string::npos);

  const Invocation invalid = Invoke(temporary.Path(), {"build", "windows", "--sdk", "missing"});
  REQUIRE(invalid.result == 1);
  REQUIRE(invalid.error.find("HuxerUI SDK or source checkout is invalid:") != std::string::npos);

  REQUIRE(Invoke(temporary.Path(), {"build", "--source", "."}).result == 2);
}

TEST_CASE("HuxerUICliAcceptsBuildConfigurations") {
  TemporaryDirectory temporary;
  for (const auto command : {"build", "run", "package"}) {
    const auto help = Invoke(temporary.Path(), {command, "--help"});
    REQUIRE(help.output.find("[--config debug|release]") != std::string::npos);

    for (const auto config : {"debug", "release"}) {
      const auto invocation = Invoke(temporary.Path(), {command, "windows", "--config", config, "--sdk", "missing"});
      REQUIRE(invocation.result == 1);
      REQUIRE(invocation.error.find("HuxerUI SDK or source checkout is invalid:") != std::string::npos);
    }

    const auto invalid = Invoke(temporary.Path(), {command, "windows", "--config", "Release"});
    REQUIRE(invalid.result == 2);
    REQUIRE(invalid.error.find("--config must be debug or release") != std::string::npos);
  }
}

TEST_CASE("HuxerUICliEnablesProfilingForSourceBuilds") {
  TemporaryDirectory temporary;
  for (const auto command : {"build", "run", "package"}) {
    REQUIRE(Invoke(temporary.Path(), {command, "--help"}).output.find("--profile enables profiling") !=
            std::string::npos);

    const auto missing = Invoke(temporary.Path(), {command, "windows", "--profile"});
    REQUIRE(missing.result == 2);
    REQUIRE(missing.error.find("--profile requires --sdk <path>") != std::string::npos);
    const auto invalid = Invoke(temporary.Path(), {command, "windows", "--profile", "--sdk", "."});
    REQUIRE(invalid.result == 2);
    REQUIRE(invalid.error.find("pointing to a HuxerUI source checkout") != std::string::npos);

    const auto source = Invoke(temporary.Path(), {command, "windows", "--profile", "--sdk", HUXERUI_TEST_SOURCE_DIRECTORY});
    REQUIRE(source.result == 1);
    REQUIRE(source.error.find("no HuxerUI project found") != std::string::npos);
  }
  REQUIRE(Invoke(temporary.Path(), {"build", "windows", "--profile", "release"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"build", "windows", "--profile", "--profile"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"open", "ios", "--profile"}).result == 2);
}

TEST_CASE("HuxerUICliShortCommandsShareDispatchAndHelp") {
  TemporaryDirectory temporary;
  const auto general_help = Invoke(temporary.Path(), {"--help"});
  for (const auto& [alias, command] : std::array{
           std::pair{"b", "build"}, std::pair{"r", "run"}, std::pair{"pkg", "package"},
       }) {
    const auto help = Invoke(temporary.Path(), {command, "--help"});
    REQUIRE(help.result == 0);
    REQUIRE(help.output.find(std::string("huxerui ") + command + " ") != std::string::npos);
    REQUIRE(Invoke(temporary.Path(), {alias, "-h"}).output == help.output);
    REQUIRE(general_help.output.find(std::string(alias) + "  " + command) != std::string::npos);

    const auto full = Invoke(temporary.Path(), {command, "windows", "--sdk", "missing"});
    const auto short_form = Invoke(temporary.Path(), {alias, "windows", "--sdk", "missing"});
    REQUIRE(short_form.result == full.result);
    REQUIRE(short_form.error == full.error);
  }
  REQUIRE(Invoke(temporary.Path(), {"open", "--help"}).output.find("open ios [--sdk <path>]") != std::string::npos);
}

TEST_CASE("HuxerUICliRestrictsWatchToRun") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"run", "--help"}).output.find("[--watch]") != std::string::npos);
  REQUIRE(Invoke(temporary.Path(), {"package", "windows", "--watch"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"build", "windows", "--watch"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"open", "ios", "--watch"}).result == 2);
  const auto duplicate = Invoke(temporary.Path(), {"run", "windows", "--watch", "--watch"});
  REQUIRE(duplicate.result == 2);
  REQUIRE(duplicate.error.find("--watch may be specified only once") != std::string::npos);
  const auto selected = Invoke(temporary.Path(), {"r", "windows", "--watch", "--sdk", "missing"});
  REQUIRE(selected.result == 1);
  REQUIRE(selected.error.find("HuxerUI SDK or source checkout is invalid:") != std::string::npos);
}

TEST_CASE("HuxerUICliDoctorReportsIncompleteAndUnknownPlatforms") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "sample", "--platform", "windows"}).result == 0);
  const std::filesystem::path project = temporary.Path() / "sample";
  std::filesystem::remove(project / "platform/windows/app.manifest");
  std::filesystem::create_directories(project / "platform/custom");

  const Invocation invocation = Invoke(project, {"doctor", "all"});

  REQUIRE(invocation.result == 1);
  REQUIRE(invocation.output.find("unknown platform directory: custom") != std::string::npos);
  REQUIRE(invocation.output.find("missing app.manifest") != std::string::npos);
  REQUIRE(invocation.output.find("Platform android") == std::string::npos);
}

TEST_CASE("HuxerUICliRejectsInvalidOptimizationOverrides") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"build", "android", "--optimization"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"build", "android", "--optimization", "fast"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"build", "android", "--lto"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"build", "android", "--lto", "auto"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"build", "android", "--lto", "off", "--lto", "on"}).result == 2);
}

TEST_CASE("HuxerUICliRestrictsPackageFormatToLinuxPackaging") {
  TemporaryDirectory temporary;
  const auto help = Invoke(temporary.Path(), {"package", "--help"});
  REQUIRE(help.output.find("[-f|--format appimage|deb|rpm]") != std::string::npos);
  REQUIRE(Invoke(temporary.Path(), {"package", "linux", "-f"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"package", "linux", "--format", "zip"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"pkg", "linux", "-f", "deb", "--format", "rpm"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"build", "linux", "--format", "deb"}).result == 2);

  REQUIRE(Invoke(temporary.Path(), {"create", "app", "sample"}).result == 0);
  const auto project = temporary.Path() / "sample";
  for (const auto platforms : {"windows", "linux,windows"}) {
    const auto rejected =
        Invoke(project, {"package", platforms, "--format", "rpm", "--sdk", HUXERUI_TEST_SOURCE_DIRECTORY});
    REQUIRE(rejected.result == 2);
    REQUIRE(rejected.error.find("--format requires Linux as the sole packaging platform") != std::string::npos);
  }
}

TEST_CASE("HuxerUICliNormalizesAbiNamesForTheSelectedPlatform") {
  using huxerui::cli::FindPlatformDriver;
  using huxerui::cli::detail::NormalizeTargetAbi;

  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("windows"), "x64") == "x86_64");
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("windows"), "x86_64") == "x86_64");
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("windows"), "arm64").empty());
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("android"), "arm64") == "arm64-v8a");
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("android"), "arm64-v8a") == "arm64-v8a");
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("android"), "x64") == "x86_64");
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("linux"), "arm64") == "aarch64");
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("macos"), "aarch64") == "arm64");
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("ios"), "x64") == "x86_64");
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("web"), "wasm32") == "wasm32");
  REQUIRE(NormalizeTargetAbi(*FindPlatformDriver("web"), "wasm64").empty());
}

TEST_CASE("HuxerUICliRejectsInvalidAbiSelectionsBeforeBuilding") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"build", "android", "--abi"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"build", "android", "--abi", "arm64,"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"build", "android", "--abi", "arm64", "--abi", "x64"}).result == 2);
  REQUIRE(Invoke(temporary.Path(), {"open", "ios", "--abi", "arm64"}).result == 2);

  REQUIRE(Invoke(temporary.Path(), {"create", "app", "sample"}).result == 0);
  const auto project = temporary.Path() / "sample";
  const auto unsupported =
      Invoke(project, {"build", "web", "--abi", "wasm64", "--sdk", HUXERUI_TEST_SOURCE_DIRECTORY});
  REQUIRE(unsupported.result == 2);
  REQUIRE(unsupported.error.find("unsupported ABI for web: wasm64") != std::string::npos);
  REQUIRE(Invoke(project, {"build", "android,web", "--abi", "arm64", "--sdk", HUXERUI_TEST_SOURCE_DIRECTORY})
              .result == 2);
  REQUIRE(Invoke(project, {"run", "linux", "--abi", "x64,arm64", "--sdk", HUXERUI_TEST_SOURCE_DIRECTORY})
              .result == 2);
}

TEST_CASE("HuxerUICliRejectsUnknownPlatformsAsUsageErrors") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"create", "app", "sample", "--platform", "plan9"});

  REQUIRE(invocation.result == 2);
  REQUIRE(invocation.error.find("unknown platform: plan9") != std::string::npos);
  REQUIRE_FALSE(std::filesystem::exists(temporary.Path() / "sample"));
}

TEST_CASE("HuxerUICliRejectsInvalidAgentLists") {
  TemporaryDirectory temporary;
  const Invocation unknown =
      Invoke(temporary.Path(), {"create", "app", "unknown", "--platform", "windows", "--agent", "other"});
  REQUIRE(unknown.result == 2);
  REQUIRE(unknown.error.find("unknown agent: other") != std::string::npos);

  const Invocation combined = Invoke(
      temporary.Path(),
      {"create", "app", "combined", "--platform", "windows", "--agent", "all,claude"}
  );
  REQUIRE(combined.result == 2);
  REQUIRE(combined.error.find("all cannot be combined with another agent") != std::string::npos);

  const Invocation disabled = Invoke(
      temporary.Path(),
      {"create", "app", "disabled", "--platform", "windows", "--agent", "none,codex"}
  );
  REQUIRE(disabled.result == 2);
  REQUIRE(disabled.error.find("none cannot be combined with another agent") != std::string::npos);
}

TEST_CASE("HuxerUICliRejectsDeviceDiscoveryForDesktopPlatforms") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"devices", "windows"});

  REQUIRE(invocation.result == 1);
  REQUIRE(invocation.error.find("platform does not support device discovery: windows") != std::string::npos);
}

TEST_CASE("HuxerUICliRequiresAValueForJavaHomeOverrides") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"build", "android", "--java-home"});

  REQUIRE(invocation.result == 2);
  REQUIRE(invocation.error.find("--java-home requires a value") != std::string::npos);
}

TEST_CASE("HuxerUICliRejectsJavaHomeOverridesForNonAndroidBuilds") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "sample", "--platform", "windows"}).result == 0);
  const std::filesystem::path project = temporary.Path() / "sample";
  const std::vector<std::string_view> arguments{
      "build", "windows", "--java-home", "jdk", "--sdk", HUXERUI_TEST_SOURCE_DIRECTORY
  };
  std::ostringstream output;
  std::ostringstream error;
  std::istringstream input;

  const int result = huxerui::cli::Run(
      arguments,
      project,
      {HUXERUI_TEST_SOURCE_DIRECTORY, huxerui::cli::SdkLocationSource::Executable},
      input,
      output,
      error
  );

  REQUIRE(result == 2);
  REQUIRE(error.str().find("--java-home is supported only for Android builds") != std::string::npos);
  REQUIRE(output.str().empty());
}

TEST_CASE("HuxerUICliRejectsUnknownProjectPlatformsBeforeRun") {
  TemporaryDirectory temporary;
  REQUIRE(Invoke(temporary.Path(), {"create", "app", "sample", "--platform", "windows"}).result == 0);
  const std::filesystem::path project = temporary.Path() / "sample";
  std::filesystem::create_directories(project / "platform/custom");
  const std::vector<std::string_view> arguments{"run", "windows", "--sdk", HUXERUI_TEST_SOURCE_DIRECTORY};
  std::ostringstream output;
  std::ostringstream error;
  std::istringstream input;

  const int result = huxerui::cli::Run(
      arguments,
      project,
      {temporary.Path(), huxerui::cli::SdkLocationSource::Executable},
      input,
      output,
      error
  );

  REQUIRE(result == 1);
  REQUIRE(error.str().find("unknown platform directory: custom") != std::string::npos);
  REQUIRE(output.str().empty());
}

TEST_CASE("HuxerUICliSetupRequiresAnExplicitPlatformList") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"setup"});

  REQUIRE(invocation.result == 2);
  REQUIRE(invocation.error.find("setup requires an explicit platform list") != std::string::npos);
}

TEST_CASE("HuxerUICliSetupUsesSharedDiagnosisAndCanBeCancelled") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"setup", "android"});

  REQUIRE(invocation.result == 1);
  REQUIRE(invocation.output.find("Common environment:") != std::string::npos);
  REQUIRE(invocation.output.find("Platform android:") != std::string::npos);
  REQUIRE(invocation.output.find("Setup plan:") != std::string::npos);
  REQUIRE(invocation.output.find("Setup cancelled.") != std::string::npos);
  REQUIRE(invocation.error.empty());
}

TEST_CASE("HuxerUICliPlatformEnvironmentDiagnosisOwnsHostAndToolChecks") {
  const std::string_view unavailable_id = huxerui::cli::CurrentHostId() == "windows" ? "ios" : "windows";
  const huxerui::cli::PlatformDriver* unavailable = huxerui::cli::FindPlatformDriver(unavailable_id);
  REQUIRE(unavailable != nullptr);
  const std::vector<huxerui::cli::EnvironmentDiagnostic> unavailable_diagnostics = unavailable->DiagnoseEnvironment();
  REQUIRE(unavailable_diagnostics.size() == 1);
  REQUIRE(unavailable_diagnostics.front().status == huxerui::cli::EnvironmentDiagnosticStatus::Unavailable);
  REQUIRE(unavailable_diagnostics.front().id == "host");
}

TEST_CASE("HuxerUICliDoctorChecksRequestedPlatformsOutsideAProject") {
  TemporaryDirectory temporary;
  const Invocation invocation = Invoke(temporary.Path(), {"doctor", "android"});

  REQUIRE(invocation.result == 1);
  REQUIRE(invocation.output.find("Project: not found") != std::string::npos);
  REQUIRE(invocation.output.find("Platform android:") != std::string::npos);
  REQUIRE(invocation.output.find("  [ok] cmake") == std::string::npos);
}
