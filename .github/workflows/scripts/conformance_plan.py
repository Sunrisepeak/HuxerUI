"""Selects the conformance jobs a change can affect, for build-system-conformance.yml.

Reads changed paths, one per line, from standard input and writes GitHub step outputs: `integrations`, a JSON list of
integration names; `rows` and `macos_rows`, the JSON matrix rows of the conformance jobs; and `compare`,
`compare_macos` and `sequence`, whether those jobs run.

Two independent selections narrow a pull request's run.

- Integrations: a path an integration owns selects that integration; every other path selects all of them.
- Rows: a path one target owns selects that target on every host it is built on. A path of the build tooling, which
  runs on the host, selects every target on every host. Any other path is source compiled for the target, which
  selects every target on its primary host only: the target's own host, or a Linux host for a target that is not a
  host. The remaining hosts run on `main` and on a manual dispatch.

Standard input closed without a path, as for a push or a dispatch, selects everything.
"""

import json
import sys
from fnmatch import fnmatch
from pathlib import PurePosixPath


INTEGRATIONS = ("cmake", "mcpp")

# The rows of the Linux and Windows job and of the macOS job, in the matrix's form. CMake's macOS rows use Intel
# runners because GitHub's macOS arm64 runners are capacity constrained; mcpp's Apple toolchain is arm64 only.
ROWS = (
    {"target": "linux", "host": "linux", "os": "ubuntu-24.04", "python": "python3"},
    {"target": "windows", "host": "windows", "os": "windows-2022", "python": "python"},
    {"target": "web", "host": "linux", "os": "ubuntu-24.04", "python": "python3"},
    {"target": "android", "host": "linux", "os": "ubuntu-24.04", "python": "python3"},
    {"target": "web", "host": "windows", "os": "windows-2022", "python": "python"},
    {"target": "android", "host": "windows", "os": "windows-2022", "python": "python"},
)
MACOS = {"cmake": "macos-15-intel", "mcpp": "macos-15"}
MACOS_ROWS = tuple({"target": target, "host": "macos", "os": MACOS, "python": "python3"}
                   for target in ("macos", "ios", "web", "android"))
TARGETS = ("linux", "windows", "macos", "web", "android", "ios")
HOSTS = ("linux", "windows", "macos")

# The targets the sequence job builds in turn on one Linux host.
SEQUENCE_TARGETS = ("linux", "web", "android")

# Paths below these prefixes are inputs of every integration even where a file name below says otherwise:
# the CLI's own CMakeLists.txt builds the tool both integrations are driven through.
SHARED_PREFIXES = ("tools/", ".github/")

OWNED_PREFIXES = {
    "cmake": ("cmake/", "buildtools/cmake/"),
    # The templates are C++ modules sources, which only mcpp builds.
    "mcpp": ("modules/", "buildtools/mcpp/", "templates/"),
}

OWNED_NAMES = {
    "cmake": ("CMakeLists.txt", "*.cmake", "*.cmake.in"),
    "mcpp": ("mcpp.toml", "mcpp.lock", "build.mcpp", "*.cppm"),
}

# A target owns its platform directories, its CLI driver and its CMake platform file (`cmake/platform/IOS.cmake`
# belongs to `ios`).
TARGET_PATTERNS = (
    "platform/{target}/*",
    "tools/huxerui_cli/platform/{target}.cpp",
    "tools/huxerui_cli/templates/platform/{target}/*",
    "buildtools/cts/projects/*/platform/{target}/*",
)

# The build tooling runs on the host, so a change to it can behave differently on each host. The conformance
# projects below `buildtools/cts/projects/` are sources like any other.
TOOLING_PREFIXES = (".github/", "cmake/", "tools/", "buildtools/")
TOOLING_NAMES = ("CMakeLists.txt", "mcpp.toml", "mcpp.lock", "build.mcpp")
SOURCE_PREFIXES = ("buildtools/cts/projects/",)

ALL_HOSTS = "all"
PRIMARY_HOST = "primary"


def owner(path):
    """The integration that alone consumes a path, or None when the path is shared."""
    if path.startswith(SHARED_PREFIXES):
        return None
    for integration, prefixes in OWNED_PREFIXES.items():
        if path.startswith(prefixes):
            return integration
    name = PurePosixPath(path)
    for integration, patterns in OWNED_NAMES.items():
        if any(name.match(pattern) for pattern in patterns):
            return integration
    return None


def select(paths):
    """The integrations, in a stable order, that the changed paths affect."""
    selected = set()
    for path in paths:
        path = path.strip()
        if not path:
            continue
        integration = owner(path)
        if integration is None:
            return list(INTEGRATIONS)
        selected.add(integration)
    # A change no trigger path covers cannot start the workflow; an empty list still runs everything.
    return [name for name in INTEGRATIONS if name in selected] or list(INTEGRATIONS)


def target_owner(path):
    """The target that alone owns a path, or None."""
    if path.startswith("cmake/platform/") and path.endswith(".cmake"):
        target = PurePosixPath(path).stem.lower()
        return target if target in TARGETS else None
    for target in TARGETS:
        if any(fnmatch(path, pattern.format(target=target)) for pattern in TARGET_PATTERNS):
            return target
    return None


def is_tooling(path):
    if path.startswith(SOURCE_PREFIXES):
        return False
    return path.startswith(TOOLING_PREFIXES) or path in TOOLING_NAMES


def primary_host(target):
    return target if target in HOSTS else ("macos" if target == "ios" else "linux")


def select_targets(paths):
    """Each affected target, mapped to ALL_HOSTS or PRIMARY_HOST."""
    paths = [path.strip() for path in paths if path.strip()]
    if not paths:
        return dict.fromkeys(TARGETS, ALL_HOSTS)
    selected = {}
    for path in paths:
        target = target_owner(path)
        if target is not None:
            selected[target] = ALL_HOSTS
        elif is_tooling(path):
            return dict.fromkeys(TARGETS, ALL_HOSTS)
        else:
            for each in TARGETS:
                selected.setdefault(each, PRIMARY_HOST)
    return selected


def selected_rows(rows, targets):
    return [row for row in rows if row["target"] in targets
            and (targets[row["target"]] == ALL_HOSTS or row["host"] == primary_host(row["target"]))]


def outputs(paths):
    integrations = select(paths)
    targets = select_targets(paths)
    rows = selected_rows(ROWS, targets)
    macos_rows = selected_rows(MACOS_ROWS, targets)
    both = len(integrations) > 1
    return {
        "integrations": json.dumps(integrations),
        "rows": json.dumps(rows),
        "macos_rows": json.dumps(macos_rows),
        "compare": "true" if both and rows else "false",
        "compare_macos": "true" if both and macos_rows else "false",
        "sequence": "true" if any(target in targets for target in SEQUENCE_TARGETS) else "false",
    }


def main():
    for name, value in outputs(sys.stdin.read().splitlines()).items():
        print(f"{name}={value}")


if __name__ == "__main__":
    main()
