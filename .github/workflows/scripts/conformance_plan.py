"""Selects the build-system integrations a change can affect, for build-system-conformance.yml.

Reads changed paths, one per line, from standard input and writes GitHub step outputs:
`integrations`, a JSON list of integration names, and `compare`, whether the comparison jobs run.

A path an integration owns selects that integration; every other path is shared and selects all of them.
Filtering narrows by integration only, never by target, so a selected integration runs every row it has.
"""

import json
import sys
from pathlib import PurePosixPath


INTEGRATIONS = ("cmake", "mcpp")

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


def main():
    integrations = select(sys.stdin.read().splitlines())
    print(f"integrations={json.dumps(integrations)}")
    print(f"compare={'true' if len(integrations) > 1 else 'false'}")


if __name__ == "__main__":
    main()
