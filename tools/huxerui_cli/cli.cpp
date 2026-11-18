#include "cli.h"

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <fstream>
#include <istream>
#include <iterator>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "platform.h"
#include "process_runner.h"
#include "project.h"
#include "prompt.h"
#include "mcpp/mcpp.h"
#include "watch.h"

namespace huxerui::cli {
namespace {

class UsageError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

constexpr std::string_view version = HUXERUI_CLI_VERSION;
constexpr std::array<std::pair<std::string_view, std::string_view>, 3> command_aliases{{
    {"b", "build"},
    {"r", "run"},
    {"pkg", "package"},
}};

struct AgentSkillMapping {
  std::string_view id;
  AgentSkillDirectory directory;
};

constexpr std::array agent_skill_mappings{
    AgentSkillMapping{"codex", AgentSkillDirectory::Shared},
    AgentSkillMapping{"claude", AgentSkillDirectory::Claude},
    AgentSkillMapping{"antigravity", AgentSkillDirectory::Shared},
    AgentSkillMapping{"opencode", AgentSkillDirectory::Shared},
    AgentSkillMapping{"command-code", AgentSkillDirectory::Shared},
    AgentSkillMapping{"omp", AgentSkillDirectory::Shared},
    AgentSkillMapping{"dsh", AgentSkillDirectory::Shared},
    AgentSkillMapping{"zcode", AgentSkillDirectory::ZCode},
};

constexpr std::string_view build_options =
    "[--build-system cmake|mcpp] [-d|--device <id>] [--config debug|release] [--generator <name>] [--sdk <path>] "
    "[--java-home <path>] [--no-shell]";
constexpr std::string_view package_format_option = " [-f|--format appimage|deb|rpm]";
constexpr std::string_view watch_option_help = "--watch rebuilds and relaunches the selected application after edits.\n";
constexpr std::string_view sdk_option_help =
    "--sdk <path> selects an installed SDK or a HuxerUI source checkout for this invocation.\n";
constexpr std::string_view sdk_commands =
    "  huxerui sdk list\n"
    "  huxerui sdk use <version> [--yes]\n"
    "  huxerui sdk update [--check] [--yes]\n";

void PrintBuildOptions(std::ostream& output) {
  output << "A project that declares both build systems builds with cmake; --build-system mcpp selects mcpp.\n"
         << "Android and iOS build through their standard shells, Gradle and Xcode; --no-shell builds and packages\n"
         << "them with the mcpp integration alone.\n"
         << sdk_option_help
         << "--config defaults to debug for build and run, and release for package.\n"
         << "--profile enables profiling in a source build selected with --sdk <path>.\n"
         << "Build, run, and package accept --abi <abi-list> for a single platform.\n"
         << "They also accept --optimization default|size|minsize and --lto on|off.\n";
}

void PrintPackageOptions(std::ostream& output) {
  output << "With Linux as the sole packaging platform, -f or --format selects appimage (default), deb, or rpm.\n";
}

void PrintBuildHelp(std::ostream& output, std::string_view command) {
  output << "Usage:\n  huxerui " << command;
  if (command == "open") {
    output << " ios [--build-system cmake|mcpp] [--sdk <path>]\n\n" << sdk_option_help;
    return;
  }
  const auto platforms = command == "build" ? "[platform-list]" : command == "run" ? "<platform>" : "<platform-list>";
  output << ' ' << platforms << ' ' << build_options;
  if (command == "package") {
    output << package_format_option;
  } else if (command == "run") {
    output << " [--watch]";
  }
  output << "\n\n";
  PrintBuildOptions(output);
  if (command == "package") {
    PrintPackageOptions(output);
  } else if (command == "run") {
    output << watch_option_help;
  }
}

void PrintHelp(std::ostream& output) {
  output << "HuxerUI project and platform tool\n\n"
         << "Usage:\n"
         << "  huxerui create [app <name>] [--build-system <build-system-list>] "
            "[--template <name>] [--id <project-id>] [-p|--platform <platform-list>] [--agent <agent-list>] [--yes]\n"
         << "  huxerui create [library <name>] [--build-system <build-system-list>] "
            "[--namespace <cpp-namespace>] [--target <public-cmake-target>] "
            "[--id <project-id>] [-p|--platform <platform-list>] [--agent <agent-list>] [--yes]\n"
         << "  huxerui platform add <platform-list>\n"
         << "  huxerui doctor [platform-list]\n"
         << "  huxerui setup <platform-list> [--yes]\n"
         << sdk_commands
         << "  huxerui devices [platform]\n"
         << "  huxerui build [platform-list] " << build_options << '\n'
         << "  huxerui run <platform> " << build_options << " [--watch]\n"
         << "  huxerui package <platform-list> " << build_options << package_format_option << '\n'
         << "  huxerui open ios [--build-system cmake|mcpp] [--sdk <path>]\n"
         << "  huxerui --version\n\n";
  output << "Command aliases:\n";
  for (const auto& [alias, command] : command_aliases) {
    output << "  " << alias << "  " << command << '\n';
  }
  output << "\nA platform list is a comma-separated list or all.\n";
  PrintBuildOptions(output);
  output << watch_option_help;
  PrintPackageOptions(output);
  output << "create asks for a missing kind, name, build system, template, or platform list when run in a\n"
         << "terminal; --yes, or no terminal, applies the defaults instead. A build-system list is cmake, mcpp, or\n"
         << "cmake,mcpp: mcpp alone creates a C++ modules project (import huxerui;), and a list naming cmake a\n"
         << "headers project (#include <huxerui/huxerui.h>). An app defaults to cmake and a library to both.\n"
         << "Without --platform, an app and a common library's Preview enable all platforms.\n"
         << "An agent list is comma-separated and supports ";
  for (std::size_t index = 0; index < agent_skill_mappings.size(); ++index) {
    if (index > 0) {
      output << ", ";
    }
    output << agent_skill_mappings[index].id;
  }
  output << ", all, or none.\n";
}

std::vector<std::string_view> SplitList(std::string_view value, std::string_view kind) {
  if (value.empty()) {
    throw UsageError(std::string(kind) + " list cannot be empty");
  }

  std::vector<std::string_view> ids;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t separator = value.find(',', start);
    const std::size_t end = separator == std::string_view::npos ? value.size() : separator;
    const std::string_view id = value.substr(start, end - start);
    if (id.empty()) {
      throw UsageError(std::string(kind) + " list contains an empty " + std::string(kind));
    }
    ids.push_back(id);
    if (separator == std::string_view::npos) {
      break;
    }
    start = separator + 1;
  }
  return ids;
}

std::vector<const PlatformDriver*> ResolvePlatforms(std::string_view value) {
  const std::vector<std::string_view> requested = SplitList(value, "platform");
  if (std::find(requested.begin(), requested.end(), "all") != requested.end()) {
    if (requested.size() != 1) {
      throw UsageError("all cannot be combined with another platform");
    }
    std::vector<const PlatformDriver*> platforms;
    for (const std::string_view id : PlatformIds()) {
      platforms.push_back(FindPlatformDriver(id));
    }
    return platforms;
  }

  std::vector<const PlatformDriver*> platforms;
  for (const std::string_view id : requested) {
    const PlatformDriver* platform = FindPlatformDriver(id);
    if (!platform) {
      throw UsageError("unknown platform: " + std::string(id));
    }
    if (std::find(platforms.begin(), platforms.end(), platform) == platforms.end()) {
      platforms.push_back(platform);
    }
  }
  return platforms;
}

std::vector<AgentSkillDirectory> ResolveAgentSkillDirectories(std::string_view value) {
  const std::vector<std::string_view> requested = SplitList(value, "agent");
  const bool all = std::find(requested.begin(), requested.end(), "all") != requested.end();
  const bool none = std::find(requested.begin(), requested.end(), "none") != requested.end();
  if (all || none) {
    if (requested.size() != 1) {
      throw UsageError(std::string(all ? "all" : "none") + " cannot be combined with another agent");
    }
    if (none) {
      return {};
    }
    return {AgentSkillDirectory::Shared, AgentSkillDirectory::Claude, AgentSkillDirectory::ZCode};
  }

  std::vector<AgentSkillDirectory> directories;
  for (const std::string_view id : requested) {
    const auto mapping = std::find_if(
        agent_skill_mappings.begin(), agent_skill_mappings.end(), [id](const AgentSkillMapping& candidate) {
          return candidate.id == id;
        }
    );
    if (mapping == agent_skill_mappings.end()) {
      throw UsageError("unknown agent: " + std::string(id));
    }
    if (std::find(directories.begin(), directories.end(), mapping->directory) == directories.end()) {
      directories.push_back(mapping->directory);
    }
  }
  return directories;
}

std::optional<Project> TryDiscoverProject(const std::filesystem::path& start) {
  try {
    return DiscoverProject(start);
  } catch (const std::runtime_error&) {
    return std::nullopt;
  }
}

std::vector<EnvironmentDiagnostic> DiagnoseCommonEnvironment(const SdkLocation& sdk) {
  std::vector<EnvironmentDiagnostic> diagnostics;
  if (sdk.home.empty()) {
    diagnostics.push_back({
        EnvironmentDiagnosticStatus::Missing,
        "huxerui_home",
        "HUXERUI_HOME; install HuxerUI or set HUXERUI_HOME",
        {},
    });
  } else {
    auto status = EnvironmentDiagnosticStatus::Ready;
    std::string detail = sdk.home.string();
    try {
      static_cast<void>(ResolveBuildHome(sdk.home, std::nullopt, {}));
    } catch (const std::runtime_error& error) {
      status = EnvironmentDiagnosticStatus::Missing;
      detail += "; " + std::string(error.what());
    }
    diagnostics.push_back({
        status,
        "huxerui_home",
        "HUXERUI_HOME (" + std::string(SdkLocationSourceName(sdk.source)) + ")",
        std::move(detail),
    });
  }

  const std::optional<std::filesystem::path> cmake = FindExecutable("cmake");
  diagnostics.push_back({
      cmake ? EnvironmentDiagnosticStatus::Ready : EnvironmentDiagnosticStatus::Missing,
      "cmake",
      "cmake",
      cmake ? cmake->string() : std::string{},
  });
  return diagnostics;
}

bool PrintEnvironmentDiagnostics(std::span<const EnvironmentDiagnostic> diagnostics, std::string_view indentation,
                                 std::ostream& output) {
  bool ready = true;
  for (const EnvironmentDiagnostic& diagnostic : diagnostics) {
    std::string_view status;
    switch (diagnostic.status) {
    case EnvironmentDiagnosticStatus::Ready:
      status = "ok";
      break;
    case EnvironmentDiagnosticStatus::Missing:
      status = "missing";
      ready = false;
      break;
    case EnvironmentDiagnosticStatus::Unavailable:
      status = "unavailable";
      ready = false;
      break;
    }
    output << indentation << '[' << status << "] " << diagnostic.label;
    if (!diagnostic.detail.empty()) {
      output << ": " << diagnostic.detail;
    }
    output << '\n';
  }
  return ready;
}

void ExecuteCommands(std::span<const ProcessCommand> commands, std::ostream& output);

std::vector<SetupAction> PlanCommonSetup(std::span<const EnvironmentDiagnostic> diagnostics) {
  std::vector<SetupAction> actions;
  for (const EnvironmentDiagnostic& diagnostic : diagnostics) {
    if (diagnostic.status != EnvironmentDiagnosticStatus::Missing) {
      continue;
    }
    if (diagnostic.id == "huxerui_home") {
      actions.push_back({"Install the HuxerUI SDK or set HUXERUI_HOME", std::nullopt});
    } else if (diagnostic.id == "cmake") {
      actions.push_back({"Install CMake from https://cmake.org/download/ and add it to PATH", std::nullopt});
    }
  }
  return actions;
}

void AppendUniqueActions(std::vector<SetupAction>& destination, std::vector<SetupAction> source) {
  for (SetupAction& action : source) {
    const auto duplicate = std::find_if(destination.begin(), destination.end(), [&action](const SetupAction& existing) {
      return existing.description == action.description;
    });
    if (duplicate == destination.end()) {
      destination.push_back(std::move(action));
    }
  }
}

bool DiagnosticsReady(std::span<const EnvironmentDiagnostic> diagnostics) {
  return std::all_of(diagnostics.begin(), diagnostics.end(), [](const EnvironmentDiagnostic& diagnostic) {
    return diagnostic.status == EnvironmentDiagnosticStatus::Ready;
  });
}

int RunSetup(std::span<const std::string_view> arguments, const SdkLocation& sdk, std::istream& input,
             std::ostream& output) {
  if (arguments.size() < 2) {
    throw UsageError("setup requires an explicit platform list");
  }

  bool assume_yes = false;
  std::optional<std::string_view> platform_list;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    if (arguments[index] == "--yes") {
      if (assume_yes) {
        throw UsageError("--yes may be specified only once");
      }
      assume_yes = true;
    } else if (!platform_list) {
      platform_list = arguments[index];
    } else {
      throw UsageError("unexpected setup argument: " + std::string(arguments[index]));
    }
  }
  if (!platform_list) {
    throw UsageError("setup requires an explicit platform list");
  }

  const std::vector<const PlatformDriver*> platforms = ResolvePlatforms(*platform_list);
  const std::vector<EnvironmentDiagnostic> common_diagnostics = DiagnoseCommonEnvironment(sdk);
  output << "Common environment:\n";
  PrintEnvironmentDiagnostics(common_diagnostics, "  ", output);

  bool environment_ready = DiagnosticsReady(common_diagnostics);
  std::vector<SetupAction> actions = PlanCommonSetup(common_diagnostics);
  for (const PlatformDriver* platform : platforms) {
    const std::vector<EnvironmentDiagnostic> diagnostics = platform->DiagnoseEnvironment();
    output << "Platform " << platform->Id() << ":\n";
    PrintEnvironmentDiagnostics(diagnostics, "  ", output);
    environment_ready = DiagnosticsReady(diagnostics) && environment_ready;
    AppendUniqueActions(actions, platform->PlanSetup(diagnostics));
  }

  if (environment_ready) {
    output << "Environment is ready; no setup changes are required.\n";
    return 0;
  }
  if (actions.empty()) {
    output << "No setup action is available for the requested environment.\n";
    return 1;
  }

  output << "Setup plan:\n";
  for (const SetupAction& action : actions) {
    output << "  [" << (action.command ? "command" : "manual") << "] " << action.description << '\n';
    if (action.command) {
      output << "    > " << DescribeProcess(*action.command) << '\n';
    }
  }

  if (!assume_yes) {
    output << "Continue? [y/N] ";
    output.flush();
    std::string response;
    std::getline(input, response);
    if (response != "y" && response != "Y" && response != "yes" && response != "YES") {
      output << "Setup cancelled.\n";
      return 1;
    }
  }

  for (const SetupAction& action : actions) {
    if (!action.command) {
      output << "Manual action required: " << action.description << '\n';
      continue;
    }
    ExecuteCommands(std::span<const ProcessCommand>(&*action.command, 1), output);
  }

  output << "Post-setup diagnosis:\n";
  output << "Common environment:\n";
  const std::vector<EnvironmentDiagnostic> post_common_diagnostics = DiagnoseCommonEnvironment(sdk);
  PrintEnvironmentDiagnostics(post_common_diagnostics, "  ", output);
  environment_ready = DiagnosticsReady(post_common_diagnostics);
  for (const PlatformDriver* platform : platforms) {
    const std::vector<EnvironmentDiagnostic> diagnostics = platform->DiagnoseEnvironment();
    output << "Platform " << platform->Id() << ":\n";
    PrintEnvironmentDiagnostics(diagnostics, "  ", output);
    environment_ready = DiagnosticsReady(diagnostics) && environment_ready;
  }
  if (!environment_ready) {
    output << "Environment setup is incomplete.\n";
    return 1;
  }
  output << "Environment setup is complete.\n";
  return 0;
}

std::string JoinPlatformIds(std::span<const PlatformDriver* const> platforms) {
  std::string joined;
  for (const PlatformDriver* platform : platforms) {
    joined += (joined.empty() ? "" : ",") + std::string(platform->Id());
  }
  return joined;
}

std::string JoinBuildSystems(std::span<const BuildSystem> build_systems, std::string_view separator) {
  std::string joined;
  for (const BuildSystem build_system : build_systems) {
    joined += (joined.empty() ? "" : std::string(separator)) + std::string(BuildSystemName(build_system));
  }
  return joined;
}

template <typename T>
T RequireAnswer(std::optional<T> answer) {
  if (!answer) {
    throw std::runtime_error("project creation was cancelled");
  }
  return std::move(*answer);
}

int RunCreate(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
              const std::filesystem::path& huxerui_home, prompt::Prompter* prompter, std::ostream& output) {
  std::optional<ProjectKind> kind;
  std::optional<std::string> name;
  std::optional<std::string_view> project_id;
  std::optional<std::string_view> cpp_namespace;
  std::optional<std::string_view> public_target;
  std::optional<std::string_view> platform_list;
  std::optional<std::string_view> agent_list;
  std::vector<BuildSystem> build_systems;
  std::optional<std::string_view> template_name;
  std::string prompted_platforms;
  bool assume_defaults = false;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string_view argument = arguments[index];
    if (argument == "--yes") {
      if (assume_defaults) {
        throw UsageError("--yes may be specified only once");
      }
      assume_defaults = true;
      continue;
    }
    if (!argument.starts_with('-')) {
      if (!kind) {
        if (argument == "app") {
          kind = ProjectKind::App;
        } else if (argument == "library") {
          kind = ProjectKind::Library;
        } else {
          throw UsageError("create kind must be app or library");
        }
      } else if (!name) {
        name = std::string(argument);
      } else {
        throw UsageError("unexpected create argument: " + std::string(argument));
      }
      continue;
    }
    if (argument != "-p" && argument != "--platform" && argument != "--id" && argument != "--agent" &&
        argument != "--namespace" && argument != "--target" && argument != "--build-system" &&
        argument != "--template") {
      throw UsageError("unexpected create argument: " + std::string(argument));
    }
    if (++index >= arguments.size()) {
      throw UsageError(std::string(argument) + " requires a value");
    }
    const std::string_view value = arguments[index];
    const auto once = [&](auto& destination, std::string_view option) {
      if (destination) {
        throw UsageError(std::string(option) + " may be specified only once");
      }
    };
    if (argument == "--id") {
      once(project_id, argument);
      project_id = value;
    } else if (argument == "--namespace") {
      once(cpp_namespace, argument);
      cpp_namespace = value;
    } else if (argument == "--target") {
      once(public_target, argument);
      public_target = value;
    } else if (argument == "-p" || argument == "--platform") {
      once(platform_list, "--platform");
      platform_list = value;
    } else if (argument == "--agent") {
      once(agent_list, argument);
      agent_list = value;
    } else if (argument == "--template") {
      once(template_name, argument);
      template_name = value;
    } else {
      if (!build_systems.empty()) {
        throw UsageError("--build-system may be specified only once");
      }
      for (const std::string_view id : SplitList(value, "build system")) {
        const std::optional<BuildSystem> build_system = ParseBuildSystem(id);
        if (!build_system) {
          throw UsageError("unknown build system: " + std::string(id) + "; expected cmake or mcpp");
        }
        build_systems.push_back(*build_system);
      }
    }
  }
  if (assume_defaults) {
    prompter = nullptr;
  }

  if (!kind) {
    if (prompter == nullptr) {
      throw UsageError("create requires app or library and a project name");
    }
    const std::array<prompt::Choice, 2> kinds{{
        {"app", "an application"},
        {"library", "a reusable library with a Preview application"},
    }};
    kind = RequireAnswer(prompter->Select("Project kind", kinds, 0)) == 0 ? ProjectKind::App : ProjectKind::Library;
  }
  const auto name_problem = [&kind](std::string_view candidate) -> std::string {
    if (!IsValidProjectName(candidate)) {
      return "project name must start with a letter and contain only letters, digits, underscores, or hyphens";
    }
    if (kind == ProjectKind::Library && !IsValidLibraryProjectName(candidate)) {
      return "library name must start with a letter and contain non-empty letter or digit segments separated by '-' "
             "or '_'";
    }
    return {};
  };
  if (!name) {
    if (prompter == nullptr) {
      throw UsageError("create requires app or library and a project name");
    }
    name = RequireAnswer(prompter->Text("Project name", kind == ProjectKind::App ? "my-app" : "my-library",
                                        name_problem));
  }
  if (const std::string problem = name_problem(*name); !problem.empty()) {
    throw UsageError(problem);
  }
  if (kind != ProjectKind::Library && (cpp_namespace || public_target)) {
    throw UsageError(std::string(cpp_namespace ? "--namespace" : "--target") +
                     " is supported only for library creation");
  }

  if (prompter != nullptr && build_systems.empty()) {
    // One question selects both the build systems and, through them, the source style.
    const std::array<prompt::Choice, 3> choices{{
        {"cmake", "headers: #include <huxerui/huxerui.h>"},
        {"mcpp", "C++ modules: import huxerui;"},
        {"cmake+mcpp", "headers, with both declarations"},
    }};
    const std::size_t answer =
        RequireAnswer(prompter->Select("Build system", choices, kind == ProjectKind::App ? 0 : 2));
    if (answer != 1) {
      build_systems.push_back(BuildSystem::CMake);
    }
    if (answer != 0) {
      build_systems.push_back(BuildSystem::Mcpp);
    }
  }
  const CreateSelection selection = ResolveCreateSelection(*kind, build_systems);

  if (template_name && selection.style != SourceStyle::Modules) {
    throw UsageError("--template selects C++ modules content from templates/; it needs --build-system mcpp");
  }
  if (template_name) {
    const bool known = kind == ProjectKind::App ? *template_name == "app" || *template_name == "navigation"
                                                : *template_name == "library";
    if (!known) {
      throw UsageError("unknown template: " + std::string(*template_name) + "; expected " +
                       (kind == ProjectKind::App ? "app or navigation" : "library"));
    }
  }
  if (prompter != nullptr && !template_name && kind == ProjectKind::App && selection.style == SourceStyle::Modules) {
    const std::array<prompt::Choice, 2> templates{{
        {"app", "a page with a counter"},
        {"navigation", "pages on a NavigationStack"},
    }};
    template_name = RequireAnswer(prompter->Select("Template", templates, 0)) == 0 ? "app" : "navigation";
  }
  if (prompter != nullptr && !platform_list && kind == ProjectKind::App) {
    std::vector<prompt::Choice> choices;
    std::vector<std::size_t> all;
    for (const std::string_view id : PlatformIds()) {
      all.push_back(choices.size());
      choices.push_back({std::string(id), ""});
    }
    for (const std::size_t index : RequireAnswer(prompter->MultiSelect("Platforms", choices, all))) {
      prompted_platforms += (prompted_platforms.empty() ? "" : ",") + choices[index].label;
    }
    platform_list = prompted_platforms;
  }
  const bool platform_specified = platform_list.has_value();

  if (project_id && !IsValidProjectId(*project_id)) {
    throw UsageError("project ID must be a lowercase reverse-domain identifier with letter-prefixed segments");
  }
  if (cpp_namespace && cpp_namespace->empty()) {
    throw UsageError("--namespace requires a non-empty value");
  }
  if (public_target && public_target->empty()) {
    throw UsageError("--target requires a non-empty value");
  }
  ProjectTemplate project_template;
  try {
    project_template = kind == ProjectKind::Library
                           ? ProjectTemplate{MakeLibraryTemplateContext(*name, cpp_namespace.value_or(""),
                                 public_target.value_or(""), project_id.value_or(""))}
                           : ProjectTemplate{MakeProjectTemplateContext(*name, project_id.value_or(""))};
  } catch (const std::invalid_argument& error) {
    throw UsageError(error.what());
  }
  std::vector<const PlatformDriver*> application_platforms;
  std::vector<const PlatformDriver*> library_platforms;
  if (kind == ProjectKind::App) {
    application_platforms = ResolvePlatforms(platform_list.value_or("all"));
  } else {
    application_platforms = ResolvePlatforms(platform_list.value_or("all"));
    if (platform_specified) {
      library_platforms = application_platforms;
    }
  }
  const std::vector<AgentSkillDirectory> agent_skill_directories =
      ResolveAgentSkillDirectories(agent_list.value_or("codex"));
  const std::filesystem::path skill_source = agent_skill_directories.empty()
                                                 ? std::filesystem::path{}
                                                 : ResolveApplicationDevelopmentSkill(huxerui_home);
  const std::filesystem::path destination = working_directory / *name;
  CreateProject(destination, project_template, application_platforms, library_platforms, skill_source,
                agent_skill_directories, selection, huxerui_home, template_name.value_or(std::string_view{}));

  output << "Created " << (kind == ProjectKind::App ? "app " : "library ") << destination.string() << '\n';
  output << "Style: " << SourceStyleName(selection.style) << '\n';
  output << "Build systems: " << JoinBuildSystems(selection.build_systems, ", ") << '\n';
  if (kind == ProjectKind::App) {
    output << "Platforms:";
    for (const PlatformDriver* platform : application_platforms) {
      output << ' ' << platform->Id();
    }
    output << '\n';
  } else {
    output << "Library platforms:";
    for (const PlatformDriver* platform : library_platforms) {
      output << ' ' << platform->Id();
    }
    if (library_platforms.empty()) {
      output << " none";
    }
    output << "\nPreview platforms:";
    for (const PlatformDriver* platform : application_platforms) {
      output << ' ' << platform->Id();
    }
    output << '\n';
  }

  // The selections as options, so the same project can be created again without prompts.
  std::string command = "huxerui create " + std::string(kind == ProjectKind::App ? "app " : "library ") + *name +
                        " --build-system " + JoinBuildSystems(selection.build_systems, ",");
  if (template_name) {
    command += " --template " + std::string(*template_name);
  }
  if (platform_specified || kind == ProjectKind::App) {
    command += " --platform " + JoinPlatformIds(application_platforms);
  }
  if (project_id) {
    command += " --id " + std::string(*project_id);
  }
  if (cpp_namespace) {
    command += " --namespace " + std::string(*cpp_namespace);
  }
  if (public_target) {
    command += " --target " + std::string(*public_target);
  }
  if (agent_list) {
    command += " --agent " + std::string(*agent_list);
  }
  output << "Command: " << command << '\n';
  return 0;
}

int RunPlatform(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
                std::ostream& output) {
  if (arguments.size() != 3 || arguments[1] != "add") {
    throw UsageError("platform usage: huxerui platform add <platform-list>");
  }

  const Project project = DiscoverProject(working_directory);
  const ProjectTemplate project_template = LoadProjectTemplate(project);
  const std::vector<const PlatformDriver*> platforms = ResolvePlatforms(arguments[2]);
  AddProjectPlatforms(project, project_template, platforms);

  output << "Updated platforms:";
  for (const PlatformDriver* platform : platforms) {
    output << ' ' << platform->Id();
  }
  output << '\n';
  return 0;
}

int RunDoctor(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
              const SdkLocation& sdk, std::ostream& output) {
  if (arguments.size() > 2) {
    throw UsageError("doctor accepts at most one platform list");
  }

  bool failed = false;
  output << "HuxerUI CLI " << version << '\n';
  output << "Host: " << CurrentHostId() << '\n';
  failed = !PrintEnvironmentDiagnostics(DiagnoseCommonEnvironment(sdk), {}, output);

  std::optional<Project> project = TryDiscoverProject(working_directory);
  if (project) {
    project = ResolveApplicationProject(*project);
  }
  std::vector<const PlatformDriver*> platforms;
  if (!project) {
    output << "Project: not found\n";
    output << "Available platforms:";
    for (const std::string_view id : PlatformIds()) {
      output << ' ' << id;
    }
    output << '\n';
    if (arguments.size() == 2) {
      platforms = ResolvePlatforms(arguments[1]);
    }
  } else {
    output << "Project: " << project->root.string() << '\n';
    output << "Build systems: " << JoinBuildSystems(project->build_systems, ", ") << '\n';
    if (project->Declares(BuildSystem::Mcpp)) {
      const std::optional<std::filesystem::path> mcpp = FindExecutable("mcpp");
      output << '[' << (mcpp ? "ok" : "missing") << "] mcpp";
      if (mcpp) {
        output << ": " << mcpp->string();
      }
      output << '\n';
      failed = failed || !mcpp;
    }
    if (!std::filesystem::is_directory(project->root / "src")) {
      output << "[error] missing src directory\n";
      failed = true;
    }
    for (const std::string& id : project->unknown_platforms) {
      output << "[error] unknown platform directory: " << id << '\n';
      failed = true;
    }

    if (arguments.size() == 2 && arguments[1] != "all") {
      platforms = ResolvePlatforms(arguments[1]);
    } else {
      for (const std::string& id : project->platforms) {
        platforms.push_back(FindPlatformDriver(id));
      }
    }
    if (arguments.size() == 2) {
      for (const PlatformDriver* platform : platforms) {
        if (std::find(project->platforms.begin(), project->platforms.end(), platform->Id()) ==
            project->platforms.end()) {
          output << "[error] platform is not enabled by this project: " << platform->Id() << '\n';
          failed = true;
        }
      }
    }
  }

  for (const PlatformDriver* platform : platforms) {
    output << "Platform " << platform->Id() << ":\n";
    failed = !PrintEnvironmentDiagnostics(platform->DiagnoseEnvironment(), "  ", output) || failed;
    if (project) {
      for (const Diagnostic& diagnostic : platform->Diagnose(project->root / "platform" / platform->Id())) {
        output << (diagnostic.error ? "  [error] " : "  [ok] ") << diagnostic.message << '\n';
        failed = failed || diagnostic.error;
      }
    }
  }
  return failed ? 1 : 0;
}

void PrintDevices(const PlatformDriver& platform, std::span<const PlatformDevice> devices, std::ostream& output) {
  output << "Platform " << platform.Id() << ":\n";
  if (devices.empty()) {
    output << "  No devices found.\n";
    return;
  }
  for (const PlatformDevice& device : devices) {
    output << "  [" << DeviceStateName(device.state) << "] " << device.id;
    if (!device.name.empty()) {
      output << " (" << device.name << ')';
    }
    output << '\n';
  }
}

int RunDevices(std::span<const std::string_view> arguments, std::ostream& output) {
  if (arguments.size() > 2) {
    throw UsageError("devices accepts at most one platform");
  }

  std::vector<const PlatformDriver*> platforms;
  if (arguments.size() == 2) {
    platforms = ResolvePlatforms(arguments[1]);
    if (platforms.size() != 1) {
      throw UsageError("devices accepts exactly one platform or no platform");
    }
  } else {
    for (const std::string_view id : PlatformIds()) {
      const PlatformDriver* platform = FindPlatformDriver(id);
      if (platform->SupportsDeviceDiscovery() && platform->SupportsCurrentHost()) {
        platforms.push_back(platform);
      }
    }
  }

  if (platforms.empty()) {
    throw std::runtime_error("no device-capable platform is available from this host");
  }
  for (const PlatformDriver* platform : platforms) {
    if (!platform->SupportsDeviceDiscovery()) {
      throw std::runtime_error("platform does not support device discovery: " + std::string(platform->Id()));
    }
    if (!platform->SupportsCurrentHost()) {
      throw std::runtime_error(
          "platform " + std::string(platform->Id()) + " is unavailable from host " + std::string(CurrentHostId())
      );
    }
    PrintDevices(*platform, platform->DiscoverDevices(), output);
  }
  return 0;
}

struct BuildOptions {
  std::optional<std::string_view> platforms;
  std::optional<std::filesystem::path> sdk_override;
  std::optional<std::filesystem::path> java_home;
  std::string profile = "debug";
  std::string device;
  std::string cmake_generator;
  std::optional<PlatformDevice> selected_device;
  bool profile_explicit = false;
  bool package = false;
  std::optional<std::string> optimization;
  std::optional<bool> lto;
  bool profiling = false;
  bool watch = false;
  std::vector<std::string> abis;
  std::optional<std::string> package_format;
  std::optional<BuildSystem> build_system;
  bool no_shell = false;
};

BuildOptions ParseBuildOptions(std::span<const std::string_view> arguments, std::string_view command) {
  BuildOptions options;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string_view option = arguments[index];
    const auto read_value = [&]() {
      if (++index == arguments.size() || arguments[index].empty() || arguments[index].starts_with('-')) {
        throw UsageError(std::string(option) + " requires a value");
      }
      return arguments[index];
    };
    if (option == "--config") {
      options.profile = read_value();
      options.profile_explicit = true;
      if (options.profile != "debug" && options.profile != "release") {
        throw UsageError("--config must be debug or release");
      }
    } else if (option == "--profile" && command != "open") {
      if (options.profiling) {
        throw UsageError("--profile may be specified only once");
      }
      options.profiling = true;
    } else if (option == "--watch" && command == "run") {
      if (options.watch) throw UsageError("--watch may be specified only once");
      options.watch = true;
    } else if (option == "--abi") {
      if (!options.abis.empty()) {
        throw UsageError("--abi may be specified only once");
      }
      const auto abis = SplitList(read_value(), "ABI");
      options.abis.assign(abis.begin(), abis.end());
    } else if (command == "package" && (option == "--format" || option == "-f")) {
      if (options.package_format) {
        throw UsageError("--format may be specified only once");
      }
      const auto format = read_value();
      if (format != "appimage" && format != "deb" && format != "rpm") {
        throw UsageError("Linux package format must be appimage, deb, or rpm");
      }
      options.package_format = format;
    } else if (option == "--optimization") {
      if (options.optimization) {
        throw UsageError("--optimization may be specified only once");
      }
      const auto optimization = read_value();
      if (optimization != "default" && optimization != "size" && optimization != "minsize") {
        throw UsageError("optimization must be default, size, or minsize");
      }
      options.optimization = optimization;
    } else if (option == "--lto") {
      if (options.lto) {
        throw UsageError("--lto may be specified only once");
      }
      const auto lto = read_value();
      if (lto != "on" && lto != "off") {
        throw UsageError("lto must be on or off");
      }
      options.lto = lto == "on";
    } else if (option == "--device" || option == "-d") {
      if (!options.device.empty()) {
        throw UsageError("--device may be specified only once");
      }
      options.device = read_value();
    } else if (option == "--build-system") {
      if (options.build_system) {
        throw UsageError("--build-system may be specified only once");
      }
      const auto name = read_value();
      options.build_system = ParseBuildSystem(name);
      if (!options.build_system) {
        throw UsageError("unknown build system: " + std::string(name) + "; expected cmake or mcpp");
      }
    } else if (option == "--no-shell" && command != "open") {
      if (options.no_shell) {
        throw UsageError("--no-shell may be specified only once");
      }
      options.no_shell = true;
    } else if (option == "--generator") {
      options.cmake_generator = read_value();
    } else if (option == "--sdk") {
      if (options.sdk_override) {
        throw UsageError("--sdk may be specified only once");
      }
      options.sdk_override = std::filesystem::path(read_value());
    } else if (option == "--java-home") {
      if (options.java_home) {
        throw UsageError("--java-home may be specified only once");
      }
      options.java_home = std::filesystem::path(read_value());
    } else if (option.starts_with('-')) {
      throw UsageError("unknown option: " + std::string(option));
    } else if (!options.platforms) {
      options.platforms = option;
    } else {
      throw UsageError("unexpected argument: " + std::string(option));
    }
  }
  if (!command.empty() && !options.platforms) {
    throw UsageError(std::string(command) + " requires one platform");
  }
  return options;
}

std::filesystem::path ResolveAndExportBuildHome(const std::filesystem::path& sdk_home,
                                                const BuildOptions& options,
                                                const std::filesystem::path& working_directory) {
  if (options.profiling && (!options.sdk_override || !IsSourceHome(working_directory / *options.sdk_override))) {
    throw UsageError("--profile requires --sdk <path> pointing to a HuxerUI source checkout");
  }
  const std::filesystem::path huxerui_home = ResolveBuildHome(sdk_home, options.sdk_override, working_directory);
  SetProcessEnvironmentVariable("HUXERUI_HOME", huxerui_home.string());
  return huxerui_home;
}

/// How a command treats the standard platform shells under `platform/<platform-id>`.
enum class ShellUse {
  /// The command drives the shell, so a selected platform must be enabled and its shell complete.
  Required,
  /// The integration builds without the standard shell, selected explicitly with `--no-shell`; its build rules read
  /// whatever shell inputs exist and report what they lack.
  Optional,
};

/// Whether a platform's application core is consumed by a standard shell that builds and packages the application:
/// Gradle on Android and Xcode on iOS. Every integration builds these platforms through the shell by default.
bool HasCoreShell(const PlatformDriver& platform) {
  return platform.Id() == "android" || platform.Id() == "ios";
}

std::vector<const PlatformDriver*> ResolveBuildPlatforms(const Project& project, BuildOptions& options,
                                                         std::string_view command,
                                                         ShellUse shells = ShellUse::Required) {
  std::vector<const PlatformDriver*> platforms;
  if (!options.platforms) {
    const PlatformDriver* current = FindPlatformDriver(CurrentHostId());
    if (!current ||
        std::find(project.platforms.begin(), project.platforms.end(), current->Id()) == project.platforms.end()) {
      throw UsageError("build requires a platform when the current host platform is not enabled");
    }
    platforms.push_back(current);
  } else if (*options.platforms == "all") {
    for (const std::string& id : project.platforms) {
      platforms.push_back(FindPlatformDriver(id));
    }
  } else {
    platforms = ResolvePlatforms(*options.platforms);
  }

  if (!command.empty() && platforms.size() != 1) {
    throw UsageError(std::string(command) + " accepts exactly one platform");
  }
  if (options.package_format && (platforms.size() != 1 || platforms.front()->Id() != "linux")) {
    throw UsageError("--format requires Linux as the sole packaging platform");
  }
  if (!options.abis.empty()) {
    if (platforms.size() != 1) {
      throw UsageError("--abi requires exactly one build platform");
    }
    const PlatformDriver& platform = *platforms.front();
    for (std::string& abi : options.abis) {
      const std::string_view normalized = detail::NormalizeTargetAbi(platform, abi);
      if (normalized.empty()) {
        throw UsageError("unsupported ABI for " + std::string(platform.Id()) + ": " + abi);
      }
      abi = normalized;
    }
    std::ranges::sort(options.abis);
    options.abis.erase(std::unique(options.abis.begin(), options.abis.end()), options.abis.end());
    if (command == "run" && platform.Id() == "linux" && options.abis.size() > 1) {
      throw UsageError("run linux accepts one ABI");
    }
  }
  if (!options.device.empty()) {
    if (platforms.size() != 1) {
      throw UsageError("--device requires exactly one build platform");
    }
    if (!platforms.front()->SupportsDeviceDiscovery()) {
      throw UsageError("--device is not supported for platform " + std::string(platforms.front()->Id()));
    }
  }
  const bool builds_android = std::ranges::any_of(platforms, [](const PlatformDriver* platform) {
    return platform->Id() == "android";
  });
  if (options.java_home && !builds_android) {
    throw UsageError("--java-home is supported only for Android builds");
  }
  for (const PlatformDriver* platform : platforms) {
    if (shells == ShellUse::Required &&
        std::find(project.platforms.begin(), project.platforms.end(), platform->Id()) == project.platforms.end()) {
      throw std::runtime_error("platform is not enabled by this project: " + std::string(platform->Id()));
    }
    if (!platform->SupportsCurrentHost()) {
      throw std::runtime_error(
          "platform " + std::string(platform->Id()) + " cannot be built from host " + std::string(CurrentHostId())
      );
    }
    if (shells == ShellUse::Optional) {
      continue;
    }
    for (const Diagnostic& diagnostic : platform->Diagnose(project.root / "platform" / platform->Id())) {
      if (diagnostic.error) {
        throw std::runtime_error("platform " + std::string(platform->Id()) + ": " + diagnostic.message);
      }
    }
  }
  return platforms;
}

void ResolveJavaHome(BuildOptions& options, const std::filesystem::path& working_directory) {
  if (!options.java_home) {
    return;
  }
  const std::filesystem::path java_home =
      (options.java_home->is_absolute() ? *options.java_home : working_directory / *options.java_home)
          .lexically_normal();
  if (!std::filesystem::is_directory(java_home)) {
    throw std::runtime_error("Java home is not a directory: " + java_home.string());
  }
#if defined(_WIN32)
  constexpr std::string_view java_executable = "java.exe";
#else
  constexpr std::string_view java_executable = "java";
#endif
  if (!std::filesystem::is_regular_file(java_home / "bin" / java_executable)) {
    throw std::runtime_error("Java home does not contain bin/" + std::string(java_executable) + ": " +
                             java_home.string());
  }
  options.java_home = java_home;
}

PlatformCommandContext MakeCommandContext(const Project& project, const PlatformDriver& platform,
                                          const std::filesystem::path& huxerui_home, const BuildOptions& options,
                                          std::span<const std::string> abis = {}) {
  std::string build_variant(platform.Id());
  if (platform.Id() == "ios") {
    build_variant +=
        options.selected_device && options.selected_device->kind == DeviceKind::Physical ? "-device" : "-simulator";
  }
  if (!abis.empty()) {
    build_variant += "-" + detail::JoinTargetAbis(abis, '+');
  }
  std::string build_profile = options.profile;
  if (options.optimization) {
    build_profile += "-" + *options.optimization;
  }
  if (options.lto) {
    build_profile += *options.lto ? "-lto" : "-no-lto";
  }
  if (options.profiling) {
    build_profile += "-profiling";
  }
  if (options.watch) build_profile += "-watch";
  const std::filesystem::path build_directory =
      project.root / ".huxerui/build" / BuildHomeKey(huxerui_home) / build_variant / build_profile;
  std::string cmake_generator = options.cmake_generator;
  if (platform.Id() == "windows") {
    if (!cmake_generator.empty()) {
      throw UsageError("Windows builds select an MSVC-compatible CMake generator automatically");
    }
    if (!std::filesystem::is_regular_file(build_directory / "CMakeCache.txt")) {
      cmake_generator = FindExecutable("ninja") ? "Ninja" : "NMake Makefiles";
    }
  } else if (
      platform.Id() != "android" && platform.Id() != "ios" && cmake_generator.empty() &&
      !std::filesystem::is_regular_file(build_directory / "CMakeCache.txt") &&
      !ReadEnvironmentVariable("CMAKE_GENERATOR") && FindExecutable("ninja")
  ) {
    cmake_generator = "Ninja";
  }
  return {
      project.root,
      huxerui_home,
      build_directory,
      std::move(cmake_generator),
      options.profile,
      options.selected_device,
      options.package,
      options.optimization,
      options.lto,
      {abis.begin(), abis.end()},
      options.package_format,
      options.profiling,
      options.watch,
  };
}

std::vector<PlatformCommandContext> MakeBuildContexts(const Project& project, const PlatformDriver& platform,
                                                      const std::filesystem::path& huxerui_home,
                                                      const BuildOptions& options) {
  std::vector<PlatformCommandContext> contexts;
  if (platform.Id() == "linux" && options.abis.size() > 1) {
    for (const std::string& abi : options.abis) {
      contexts.push_back(MakeCommandContext(project, platform, huxerui_home, options, std::span(&abi, 1)));
    }
  } else {
    contexts.push_back(MakeCommandContext(project, platform, huxerui_home, options, options.abis));
  }
  for (const PlatformCommandContext& context : contexts) {
    platform.ValidateBuildAbis(context);
  }
  return contexts;
}

void ExecuteCommands(std::span<const ProcessCommand> commands, std::ostream& output) {
  for (const ProcessCommand& command : commands) {
    output << "> " << DescribeProcess(command) << '\n';
    output.flush();
    const int result = RunProcess(command);
    if (result != 0) {
      throw std::runtime_error(
          "command failed with exit code " + std::to_string(result) + ": " + DescribeProcess(command)
      );
    }
  }
}

void BuildPlatform(const PlatformDriver& platform, const PlatformCommandContext& context,
                   const BuildOptions& options, std::ostream& output) {
  output << "Building " << platform.Id() << " (" << context.profile;
  if (!context.abis.empty()) {
    output << ", " << detail::JoinTargetAbis(context.abis, ',');
  }
  output << ")\n";
  platform.PrepareBuildEnvironment();
  auto library_graph = platform.LibraryGraphCommands(context);
  if (options.watch && library_graph.empty()) library_graph = detail::LibraryGraphConfigureCommands(context);
  ExecuteCommands(library_graph, output);
  platform.UpdateProjectIntegration(context);
  if (platform.Id() == "android" && options.java_home) {
    output << "Java home: " << options.java_home->string() << '\n';
    SetProcessEnvironmentVariable("JAVA_HOME", options.java_home->string());
  }
  ExecuteCommands(platform.BuildCommands(context), output);
  platform.VerifyBuildAbis(context);
}

std::optional<PlatformDevice> SelectDevice(const PlatformDriver& platform, std::string_view requested) {
  if (!platform.SupportsDeviceDiscovery()) {
    if (!requested.empty()) {
      throw UsageError("--device is not supported for platform " + std::string(platform.Id()));
    }
    return std::nullopt;
  }

  const std::vector<PlatformDevice> devices = platform.DiscoverDevices();
  if (!requested.empty()) {
    const auto selected = std::find_if(devices.begin(), devices.end(), [requested](const PlatformDevice& device) {
      return device.id == requested;
    });
    if (selected == devices.end()) {
      throw std::runtime_error(
          "device " + std::string(requested) + " was not found for platform " + std::string(platform.Id())
      );
    }
    if (selected->state != DeviceState::Ready) {
      throw std::runtime_error("device " + selected->id + " is " + std::string(DeviceStateName(selected->state)));
    }
    return *selected;
  }

  std::vector<PlatformDevice> ready;
  std::copy_if(devices.begin(), devices.end(), std::back_inserter(ready), [](const PlatformDevice& device) {
    return device.state == DeviceState::Ready;
  });
  if (ready.empty()) {
    throw std::runtime_error(
        "no ready devices found for platform " + std::string(platform.Id()) + "; run 'huxerui devices " +
        std::string(platform.Id()) + "' for details"
    );
  }
  if (ready.size() > 1) {
    throw std::runtime_error(
        "multiple ready devices found for platform " + std::string(platform.Id()) + "; use --device <id>"
    );
  }
  return ready.front();
}

/// The integration a command dispatches to: the one `--build-system` selects, the project's only one, or CMake for a
/// project that declares both.
BuildSystem SelectBuildSystem(const Project& project, const BuildOptions& options) {
  if (options.build_system) {
    if (!project.Declares(*options.build_system)) {
      throw UsageError("this project has no " + std::string(BuildSystemName(*options.build_system)) +
                       " declaration: " + project.root.string());
    }
    return *options.build_system;
  }
  // `project.build_systems` is in `BuildSystem` order, so a project that declares both starts with CMake.
  return project.build_systems.front();
}

/// The project a build command dispatches to mcpp, or nothing when CMake builds it or no project is found;
/// the CMake path then reports a missing project after resolving the SDK, as it always has.
std::optional<Project> FindMcppProject(const std::filesystem::path& working_directory, const BuildOptions& options) {
  std::optional<Project> project = TryDiscoverProject(working_directory);
  if (!project) {
    return std::nullopt;
  }
  project = ResolveApplicationProject(*project);
  if (SelectBuildSystem(*project, options) != BuildSystem::Mcpp) {
    return std::nullopt;
  }
  return project;
}

/// Applies the contract's rule for options the mcpp integration cannot honor: an option that would change the
/// product is invalid usage, and an option that only steers the CMake driver is reported and not applied.
/// `through_shell` is true when every selected platform builds through its standard shell, which takes the ABIs,
/// the device, and the JDK as it does for CMake.
void ApplyMcppOptionRules(BuildOptions& options, bool through_shell, std::ostream& error) {
  const auto refuse = [](std::string_view option, std::string_view reason) {
    throw UsageError(std::string(option) + " " + std::string(reason) + " for an mcpp project");
  };
  if (options.sdk_override) {
    throw UsageError("--sdk does not apply to an mcpp project: it takes HuxerUI from the huxerui dependency in "
                     "mcpp.toml, so change that entry instead");
  }
  // adb-run, the runner of an Android package without the shell, takes its device from ANDROID_SERIAL.
  if (!options.device.empty() && !through_shell && !(options.no_shell && options.platforms == "android")) {
    refuse("--device", "is not available without the standard shell");
  }
  // The watcher stages the Web page the CMake build writes beside the module; `mcpp build` writes the module only.
  if (options.watch && options.platforms == "web") {
    refuse("--watch", "is not available for the Web yet");
  }
  if (options.watch && options.no_shell) {
    refuse("--watch", "is not available without the standard shell");
  }
  if (options.java_home && !through_shell) {
    error << "huxerui: warning: --java-home selects the JDK of the Gradle shell, which this command does not run; "
             "it is not applied\n";
    options.java_home.reset();
  }
}

/// The mcpp profile and features a command selects: `--optimization` and `--lto` select a release profile the
/// manifest declares, and `--profile` opens the framework's profiling feature.
mcpp::CommandOptions McppOptions(const Project& project, std::string_view platform, const BuildOptions& options) {
  try {
    mcpp::CommandOptions result = mcpp::ResolveOptions(project.root, platform, options.profile, options.optimization,
                                                       options.lto, options.profiling);
    result.abis = options.abis;
    return result;
  } catch (const std::invalid_argument& error) {
    throw UsageError(error.what());
  }
}

/// Whether the mcpp integration builds a platform through its standard shell for this command.
bool McppUsesShell(const PlatformDriver& platform, const BuildOptions& options) {
  return !options.no_shell && HasCoreShell(platform);
}

/// Resolves the platforms of an mcpp command and applies the option rules; `--no-shell` names only platforms that
/// have a standard shell to leave out.
std::vector<const PlatformDriver*> ResolveMcppPlatforms(const Project& project, BuildOptions& options,
                                                        std::string_view command,
                                                        const std::filesystem::path& working_directory,
                                                        std::ostream& error) {
  // An option that only steers the CMake driver is reported before anything can stop the command.
  if (!options.cmake_generator.empty()) {
    error << "huxerui: warning: --generator selects a CMake generator and does not apply to an mcpp project; "
             "it is not applied\n";
    options.cmake_generator.clear();
  }
  // The profile the release overrides select is a usage question, answered before the platforms are checked.
  static_cast<void>(McppOptions(project, {}, options));
  if (!project.unknown_platforms.empty()) {
    throw std::runtime_error("unknown platform directory: " + project.unknown_platforms.front());
  }
  std::vector<const PlatformDriver*> platforms =
      ResolveBuildPlatforms(project, options, command, options.no_shell ? ShellUse::Optional : ShellUse::Required);
  if (options.no_shell) {
    for (const PlatformDriver* platform : platforms) {
      if (!HasCoreShell(*platform)) {
        throw UsageError("--no-shell applies to platforms with a standard shell, android and ios; " +
                         std::string(platform->Id()) + " has none");
      }
    }
  }
  const bool through_shell = std::ranges::all_of(platforms, [&options](const PlatformDriver* platform) {
    return McppUsesShell(*platform, options);
  });
  // A standard shell builds every ABI it is given; mcpp's own path builds the ABIs it has a target for.
  if (!through_shell) {
    for (const std::string& abi : options.abis) {
      try {
        static_cast<void>(mcpp::AbiTriple(platforms.front()->Id(), abi));
      } catch (const std::invalid_argument& error) {
        throw UsageError("--abi " + abi + " is not available for an mcpp project: " + error.what());
      }
    }
  }
  ApplyMcppOptionRules(options, through_shell, error);
  ResolveJavaHome(options, working_directory);
  return platforms;
}

/// The command context of a platform the mcpp integration builds through its standard shell. The shell reads the
/// framework's location from the record the build program writes, so no SDK is resolved.
PlatformCommandContext McppShellContext(const Project& project, const PlatformDriver& platform,
                                        const BuildOptions& options) {
  PlatformCommandContext context = MakeCommandContext(project, platform, project.root, options, options.abis);
  context.build_system = BuildSystem::Mcpp;
  platform.ValidateBuildAbis(context);
  return context;
}

/// A JSON string literal of `value`.
std::string JsonQuote(std::string_view value) {
  std::string quoted = "\"";
  for (const char character : value) {
    if (character == '"' || character == '\\') {
      quoted += '\\';
    }
    quoted += character;
  }
  return quoted + '"';
}

/// Builds a host program with mcpp and writes the application-integration record the CMake build writes, which the
/// watcher stages the program from: the program, or on macOS the bundle the rules assemble beside it.
void BuildMcppProgram(const PlatformCommandContext& context, const mcpp::CommandOptions& options,
                      std::ostream& output) {
  ProcessCommand command = mcpp::BuildCommand(options);
  command.arguments.insert(command.arguments.end(), {"--message-format", "json"});
  output << "> " << DescribeProcess(command) << '\n';
  output.flush();
  const ProcessResult result = RunProcessCapture(command);
  if (result.exit_code != 0) {
    throw std::runtime_error("command failed with exit code " + std::to_string(result.exit_code) + ": " +
                             DescribeProcess(command));
  }
  std::string name;
  std::filesystem::path artifact = mcpp::ParseBuiltProgram(result.output, name).path;
  const std::filesystem::path bundle = artifact.parent_path() / (name + ".app");
  const bool bundled = std::filesystem::is_directory(bundle);
  if (bundled) {
    artifact = bundle / "Contents/MacOS" / name;
  }
  const std::filesystem::path plan = context.build_directory / "huxerui-integration/app.json";
  std::filesystem::create_directories(plan.parent_path());
  std::ofstream(plan, std::ios::binary | std::ios::trunc)
      << "{\n  \"schema\": 1,\n  \"target\": " << JsonQuote(name) << ",\n  \"artifact\": "
      << JsonQuote(artifact.generic_string()) << ",\n  \"bundle\": "
      << JsonQuote(bundled ? bundle.generic_string() : std::string{}) << "\n}\n";
}

int RunMcppCommand(const Project& project, BuildOptions& options, std::string_view command,
                   const std::filesystem::path& working_directory, std::ostream& output, std::ostream& error) {
  const std::vector<const PlatformDriver*> platforms = ResolveMcppPlatforms(
      project, options, command == "build" ? std::string_view{} : command, working_directory, error);
  if (options.watch) {
    const PlatformDriver& platform = *platforms.front();
    if (McppUsesShell(platform, options)) {
      options.selected_device = SelectDevice(platform, options.device);
      const PlatformCommandContext context = McppShellContext(project, platform, options);
      return WatchApplication(platform, context, [&] { BuildPlatform(platform, context, options, output); }, output,
                              error);
    }
    PlatformCommandContext context = MakeCommandContext(project, platform, project.root, options);
    context.build_system = BuildSystem::Mcpp;
    const mcpp::CommandOptions mcpp_options = McppOptions(project, platform.Id(), options);
    return WatchApplication(platform, context, [&] { BuildMcppProgram(context, mcpp_options, output); }, output,
                            error);
  }
  for (const PlatformDriver* platform : platforms) {
    if (McppUsesShell(*platform, options)) {
      if (command == "run") {
        options.selected_device = SelectDevice(*platform, options.device);
      }
      const PlatformCommandContext context = McppShellContext(project, *platform, options);
      BuildPlatform(*platform, context, options, output);
      if (command == "run") {
        output << "Running " << platform->Id() << '\n';
        ExecuteCommands(platform->RunCommands(context), output);
      }
      continue;
    }
    const mcpp::CommandOptions mcpp_options = McppOptions(project, platform->Id(), options);
    if (command == "run") {
      if (!options.device.empty()) {
        output << "Device: " << options.device << '\n';
        SetProcessEnvironmentVariable("ANDROID_SERIAL", options.device);
      }
      output << "Running " << platform->Id() << " (" << options.profile << ") with mcpp\n";
      const ProcessCommand run = mcpp::RunCommand(mcpp_options);
      ExecuteCommands(std::span(&run, 1), output);
      continue;
    }
    output << "Building " << platform->Id() << " (" << options.profile << ") with mcpp\n";
    const ProcessCommand build = mcpp::BuildCommand(mcpp_options);
    ExecuteCommands(std::span(&build, 1), output);
  }
  return 0;
}

void PublishPackageArtifacts(std::span<const PackageArtifact> artifacts, const std::filesystem::path& destination,
                             std::ostream& output);

/// Packages each platform: through its standard shell where it has one, and otherwise, or with `--no-shell`, with
/// `mcpp pack` in the format the platform's distributable uses. Either way the artifacts are published below
/// `dist/<platform>` through the same validated publication as the CMake path.
int RunMcppPackage(const Project& project, BuildOptions& options, const std::filesystem::path& working_directory,
                   std::ostream& output, std::ostream& error) {
  if (!options.profile_explicit) {
    options.profile = "release";
  }
  options.package = true;
  const std::vector<const PlatformDriver*> platforms =
      ResolveMcppPlatforms(project, options, {}, working_directory, error);
  for (const PlatformDriver* platform : platforms) {
    if (McppUsesShell(*platform, options)) {
      const PlatformCommandContext context = McppShellContext(project, *platform, options);
      BuildPlatform(*platform, context, options, output);
      ExecuteCommands(platform->PackageCommands(context), output);
      PublishPackageArtifacts(platform->PackageArtifacts(context), project.root / "dist" / platform->Id(), output);
      continue;
    }
    const std::string format = mcpp::PackageFormat(platform->Id(), options.package_format.value_or(""));
    const mcpp::CommandOptions mcpp_options = McppOptions(project, platform->Id(), options);
    const ProcessCommand command = mcpp::PackCommand(mcpp_options, format);
    output << "Packaging " << platform->Id() << " (" << options.profile << ", " << format << ") with mcpp\n"
           << "> " << DescribeProcess(command) << '\n';
    output.flush();
    const ProcessResult result = RunProcessCapture(command);
    if (result.exit_code != 0) {
      throw std::runtime_error("command failed with exit code " + std::to_string(result.exit_code) + ": " +
                               DescribeProcess(command));
    }
    std::vector<PackageArtifact> artifacts;
    for (const mcpp::PackedArtifact& packed : mcpp::ParsePackArtifacts(result.output)) {
      if (format != "web") {
        artifacts.push_back({packed.path, packed.path.filename()});
        continue;
      }
      // mcpp reports the Web page as the distributable; the page loads the module, its WebAssembly, and the
      // preloaded data beside it, so the directory is what is published.
      for (const std::filesystem::directory_entry& entry :
           std::filesystem::directory_iterator(packed.path.parent_path())) {
        if (entry.is_regular_file()) {
          artifacts.push_back({entry.path(), entry.path().filename()});
        }
      }
    }
    std::ranges::sort(artifacts, {}, &PackageArtifact::destination);
    PublishPackageArtifacts(artifacts, project.root / "dist" / platform->Id(), output);
  }
  return 0;
}

/// `--no-shell` selects a capability only the mcpp integration offers.
void RefuseCMakeWithoutShell(const BuildOptions& options) {
  if (options.no_shell) {
    throw UsageError("--no-shell is not available for a CMake project: the CMake integration builds and packages "
                     "Android and iOS through the standard shell");
  }
}

int RunBuild(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
             const std::filesystem::path& sdk_home, std::ostream& output, std::ostream& error) {
  BuildOptions options = ParseBuildOptions(arguments, {});
  if (const std::optional<Project> project = FindMcppProject(working_directory, options)) {
    return RunMcppCommand(*project, options, "build", working_directory, output, error);
  }
  RefuseCMakeWithoutShell(options);
  const auto huxerui_home = ResolveAndExportBuildHome(sdk_home, options, working_directory);
  const Project project = ResolveApplicationProject(DiscoverProject(working_directory));
  if (!project.unknown_platforms.empty()) {
    throw std::runtime_error("unknown platform directory: " + project.unknown_platforms.front());
  }
  const std::vector<const PlatformDriver*> platforms = ResolveBuildPlatforms(project, options, {});
  ResolveJavaHome(options, working_directory);
  if (!options.device.empty()) {
    options.selected_device = SelectDevice(*platforms.front(), options.device);
  }
  for (const PlatformDriver* platform : platforms) {
    for (const PlatformCommandContext& context : MakeBuildContexts(project, *platform, huxerui_home, options)) {
      BuildPlatform(*platform, context, options, output);
    }
  }
  return 0;
}

int RunApplication(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
                   const std::filesystem::path& sdk_home, std::ostream& output, std::ostream& error) {
  BuildOptions options = ParseBuildOptions(arguments, "run");
  if (const std::optional<Project> project = FindMcppProject(working_directory, options)) {
    return RunMcppCommand(*project, options, "run", working_directory, output, error);
  }
  RefuseCMakeWithoutShell(options);
  const auto huxerui_home = ResolveAndExportBuildHome(sdk_home, options, working_directory);
  const Project project = ResolveApplicationProject(DiscoverProject(working_directory));
  if (!project.unknown_platforms.empty()) {
    throw std::runtime_error("unknown platform directory: " + project.unknown_platforms.front());
  }
  const std::vector<const PlatformDriver*> platforms = ResolveBuildPlatforms(project, options, "run");
  ResolveJavaHome(options, working_directory);
  const PlatformDriver& platform = *platforms.front();
  const std::optional<PlatformDevice> device = SelectDevice(platform, options.device);
  if (device) {
    options.selected_device = device;
    output << "Device: " << device->id;
    if (!device->name.empty()) {
      output << " (" << device->name << ')';
    }
    output << '\n';
  }
  const auto contexts = MakeBuildContexts(project, platform, huxerui_home, options);
  const auto& context = contexts.front();
  if (options.watch) {
    return WatchApplication(platform, context, [&] { BuildPlatform(platform, context, options, output); }, output, error);
  }
  BuildPlatform(platform, context, options, output);

  output << "Running " << platform.Id() << '\n';
  const std::vector<ProcessCommand> commands = platform.RunCommands(context);
  ExecuteCommands(commands, output);
  return 0;
}

void CopyPackageArtifacts(std::span<const PackageArtifact> artifacts, const std::filesystem::path& destination_root) {
  if (artifacts.empty()) {
    throw std::runtime_error("platform did not produce any package artifacts");
  }
  std::filesystem::create_directories(destination_root);
  for (const PackageArtifact& artifact : artifacts) {
    if (artifact.destination.empty() || artifact.destination.is_absolute() ||
        std::find(artifact.destination.begin(), artifact.destination.end(), std::filesystem::path{".."}) !=
            artifact.destination.end()) {
      throw std::logic_error("platform produced an invalid package destination");
    }
    if (!std::filesystem::exists(artifact.source)) {
      throw std::runtime_error("package artifact is missing: " + artifact.source.string());
    }
    const std::filesystem::path destination = destination_root / artifact.destination;
    std::filesystem::create_directories(destination.parent_path());
    if (std::filesystem::is_directory(artifact.source)) {
      std::filesystem::copy(artifact.source, destination,
                            std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::overwrite_existing);
    } else {
      std::filesystem::copy_file(artifact.source, destination, std::filesystem::copy_options::overwrite_existing);
    }
  }
}

void PublishPackageArtifacts(std::span<const PackageArtifact> artifacts, const std::filesystem::path& destination,
                             std::ostream& output) {
  const std::filesystem::path parent = destination.parent_path();
  const std::filesystem::path staging = parent / ("." + destination.filename().string() + ".publishing");
  const std::filesystem::path previous = parent / ("." + destination.filename().string() + ".previous");
  std::error_code error;
  std::filesystem::remove_all(staging, error);
  if (error) {
    throw std::runtime_error("cannot prepare package publication: " + error.message());
  }
  if (!std::filesystem::exists(destination) && std::filesystem::exists(previous)) {
    std::filesystem::rename(previous, destination, error);
    if (error) {
      throw std::runtime_error("cannot restore previous package directory: " + error.message());
    }
  }
  std::filesystem::remove_all(previous, error);
  if (error) {
    throw std::runtime_error("cannot prepare previous package backup: " + error.message());
  }
  CopyPackageArtifacts(artifacts, staging);

  const bool had_previous = std::filesystem::exists(destination);
  if (had_previous) {
    std::filesystem::rename(destination, previous, error);
    if (error) {
      std::error_code cleanup_error;
      std::filesystem::remove_all(staging, cleanup_error);
      throw std::runtime_error("cannot preserve existing package directory: " + error.message());
    }
  }
  std::filesystem::rename(staging, destination, error);
  if (error) {
    std::string message = "cannot publish package directory: " + error.message();
    if (had_previous) {
      std::error_code restore_error;
      std::filesystem::rename(previous, destination, restore_error);
      if (restore_error) {
        message += "; previous package remains at " + previous.string() + ": " + restore_error.message();
      }
    }
    std::error_code cleanup_error;
    std::filesystem::remove_all(staging, cleanup_error);
    throw std::runtime_error(message);
  }
  if (had_previous) {
    std::filesystem::remove_all(previous, error);
    if (error) {
      output << "Warning: cannot remove previous package directory: " << error.message() << '\n';
    }
  }
  for (const PackageArtifact& artifact : artifacts) {
    output << "Packaged " << (destination / artifact.destination).string() << '\n';
  }
}

int RunPackage(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
               const std::filesystem::path& sdk_home, std::ostream& output, std::ostream& error) {
  BuildOptions options = ParseBuildOptions(arguments, "package");
  if (const std::optional<Project> project = FindMcppProject(working_directory, options)) {
    return RunMcppPackage(*project, options, working_directory, output, error);
  }
  RefuseCMakeWithoutShell(options);
  const auto huxerui_home = ResolveAndExportBuildHome(sdk_home, options, working_directory);
  if (!options.profile_explicit) {
    options.profile = "release";
  }
  options.package = true;
  const Project project = ResolveApplicationProject(DiscoverProject(working_directory));
  if (!project.unknown_platforms.empty()) {
    throw std::runtime_error("unknown platform directory: " + project.unknown_platforms.front());
  }
  const std::vector<const PlatformDriver*> platforms = ResolveBuildPlatforms(project, options, {});
  ResolveJavaHome(options, working_directory);
  if (!options.device.empty()) {
    options.selected_device = SelectDevice(*platforms.front(), options.device);
  }

  for (const PlatformDriver* platform : platforms) {
    const auto contexts = MakeBuildContexts(project, *platform, huxerui_home, options);
    std::vector<PackageArtifact> artifacts;
    for (const PlatformCommandContext& context : contexts) {
      BuildPlatform(*platform, context, options, output);
      ExecuteCommands(platform->PackageCommands(context), output);
      auto produced = platform->PackageArtifacts(context);
      for (PackageArtifact& artifact : produced) {
        if (contexts.size() > 1) {
          artifact.destination = std::filesystem::path(context.abis.front()) / artifact.destination;
        }
        artifacts.push_back(std::move(artifact));
      }
    }
    const std::filesystem::path destination = project.root / "dist" / platform->Id();
    PublishPackageArtifacts(artifacts, destination, output);
  }
  return 0;
}

int RunOpen(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
            const std::filesystem::path& sdk_home, std::ostream& output) {
  BuildOptions options = ParseBuildOptions(arguments, "open");
  if (!options.platforms || *options.platforms != "ios") {
    throw UsageError("open usage: huxerui open ios [--build-system cmake|mcpp] [--sdk <path>]");
  }
  if (options.profile_explicit || !options.device.empty() || !options.cmake_generator.empty() || options.java_home ||
      options.optimization || options.lto || !options.abis.empty()) {
    throw UsageError("open accepts only --sdk <path> and --build-system");
  }
  if (const std::optional<Project> project = FindMcppProject(working_directory, options)) {
    // The Xcode shell builds an mcpp core as it builds a CMake one; the framework is the one mcpp resolves.
    if (options.sdk_override) {
      throw UsageError("--sdk does not apply to an mcpp project: it takes HuxerUI from the huxerui dependency in "
                       "mcpp.toml, so change that entry instead");
    }
    const std::vector<const PlatformDriver*> platforms = ResolveBuildPlatforms(*project, options, "open");
    const PlatformDriver& platform = *platforms.front();
    output << "Opening ios Xcode project\n";
    const PlatformCommandContext context = McppShellContext(*project, platform, options);
    ExecuteCommands(platform.LibraryGraphCommands(context), output);
    platform.UpdateProjectIntegration(context);
    ExecuteCommands(platform.OpenCommands(context), output);
    return 0;
  }
  const auto huxerui_home = ResolveAndExportBuildHome(sdk_home, options, working_directory);
  const Project project = ResolveApplicationProject(DiscoverProject(working_directory));
  if (!project.unknown_platforms.empty()) {
    throw std::runtime_error("unknown platform directory: " + project.unknown_platforms.front());
  }
  const std::vector<const PlatformDriver*> platforms = ResolveBuildPlatforms(project, options, "open");
  const PlatformDriver& platform = *platforms.front();
  if (platform.Id() != "ios") {
    throw UsageError("open currently supports ios only");
  }
  output << "Opening ios Xcode project\n";
  const PlatformCommandContext context = MakeCommandContext(project, platform, huxerui_home, options);
  ExecuteCommands(platform.LibraryGraphCommands(context), output);
  platform.UpdateProjectIntegration(context);
  ExecuteCommands(platform.OpenCommands(context), output);
  return 0;
}

int RunSdk(std::span<const std::string_view> arguments, const SdkLocation& sdk, std::ostream& output) {
  if (arguments.empty() || (arguments.size() == 1 && (arguments[0] == "--help" || arguments[0] == "-h"))) {
    output << "Manage installed HuxerUI SDK versions.\n\nUsage:\n" << sdk_commands;
    return 0;
  }

  const auto command = arguments.front();
  arguments = arguments.subspan(1);
  if (command == "list" && arguments.empty()) {
    return RunSdkInstaller(sdk, command, {}, false, false, output);
  }
  bool check = false;
  bool yes = false;
  std::string_view target_version;
  if (command == "use") {
    if (arguments.empty()) {
      throw UsageError("sdk use requires a major.minor.patch version");
    }
    target_version = arguments.front();
    auto number = target_version;
    if (number.starts_with('v')) {
      number.remove_prefix(1);
    }
    if (std::ranges::count(number, '.') != 2 || number.starts_with('.') || number.ends_with('.') ||
        number.find_first_not_of("0123456789.") != number.npos || number.find("..") != number.npos) {
      throw UsageError("sdk use requires a major.minor.patch version");
    }
    arguments = arguments.subspan(1);
  } else if (command != "update") {
    throw UsageError("expected sdk list, sdk use <version>, or sdk update");
  }
  for (const auto argument : arguments) {
    if (command == "update" && argument == "--check" && !check) {
      check = true;
    } else if (argument == "--yes" && !yes) {
      yes = true;
    } else {
      throw UsageError("unknown or repeated sdk option: " + std::string(argument));
    }
  }
  return RunSdkInstaller(sdk, command, target_version, check, yes, output);
}

} // namespace

int Run(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
        const SdkLocation& sdk, std::istream& input, std::ostream& output, std::ostream& error,
        prompt::Prompter* prompter) {
  try {
    if (arguments.empty() || arguments[0] == "--help" || arguments[0] == "-h") {
      PrintHelp(output);
      return 0;
    }
    if (arguments[0] == "--version") {
      output << "huxerui " << version << '\n';
      return 0;
    }
    std::string_view command = arguments.front();
    for (const auto& [alias, canonical] : command_aliases) {
      if (command == alias) {
        command = canonical;
        break;
      }
    }
    if (arguments.size() == 2 && (arguments[1] == "--help" || arguments[1] == "-h") &&
        (command == "build" || command == "run" || command == "package" || command == "open")) {
      PrintBuildHelp(output, command);
      return 0;
    }
    if (command == "create") {
      return RunCreate(arguments, working_directory, sdk.home, prompter, output);
    }
    if (command == "platform") {
      return RunPlatform(arguments, working_directory, output);
    }
    if (command == "doctor") {
      return RunDoctor(arguments, working_directory, sdk, output);
    }
    if (command == "setup") {
      return RunSetup(arguments, sdk, input, output);
    }
    if (command == "sdk") {
      return RunSdk(arguments.subspan(1), sdk, output);
    }
    if (command == "devices") {
      return RunDevices(arguments, output);
    }
    if (command == "build") {
      return RunBuild(arguments, working_directory, sdk.home, output, error);
    }
    if (command == "run") {
      return RunApplication(arguments, working_directory, sdk.home, output, error);
    }
    if (command == "package") {
      return RunPackage(arguments, working_directory, sdk.home, output, error);
    }
    if (command == "open") {
      return RunOpen(arguments, working_directory, sdk.home, output);
    }
    throw UsageError("unknown command: " + std::string(arguments[0]));
  } catch (const UsageError& exception) {
    error << "huxerui: " << exception.what() << '\n';
    error << "Run 'huxerui --help' for usage.\n";
    return 2;
  } catch (const std::exception& exception) {
    error << "huxerui: " << exception.what() << '\n';
    return 1;
  }
}

} // namespace huxerui::cli
