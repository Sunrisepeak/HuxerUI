#!/usr/bin/env python3
"""Assert that mcpp.toml and the CMake build describe the same thing.

Two build systems maintaining one set of facts drift, and the drift is not
loud: adding platform/windows/win32_foo.cpp and forgetting mcpp.toml makes the
Windows mcpp build fail at link with `undefined reference`, far from the cause.

This checks the overlap that can be checked textually -- no CMake configure, so
it is fast enough to run on every push:

  * core sources          cmake/HuxerUIBuild.cmake file(GLOB)  vs  [build] sources
  * platform sources      cmake/platform/<Os>.cmake            vs  [target.*.build] sources
  * platform link libs    HUXERUI_PLATFORM_LINK_LIBRARIES      vs  [target.*.runtime] libraries
                                                                   / [runtime] frameworks
  * platform defines      HUXERUI_PLATFORM_COMPILE_DEFINITIONS vs  [target.*.build] defines

Linux libraries are deliberately NOT compared: both sides resolve them through
pkg-config, reading the same .pc files, so there is nothing to drift.
"""

from __future__ import annotations

import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# CMake platform module -> (mcpp target selector, mcpp source globs key)
PLATFORMS = {
    "Linux.cmake": "cfg(linux)",
    "Windows.cmake": "windows",
    "MacOS.cmake": "macos",
}

failures: list[str] = []


def fail(message: str) -> None:
    failures.append(message)


def cmake_list(text: str, variable: str) -> list[str]:
    """Extract a set(VARIABLE ...) list body from CMake source."""
    match = re.search(rf"set\(\s*{variable}\b(.*?)^\)", text, re.S | re.M)
    if not match:
        return []
    body = match.group(1)
    body = re.sub(r"#.*", "", body)
    # "-framework AppKit" is ONE quoted token; splitting on whitespace would
    # turn it into two and make every framework name look unmatched.
    tokens = re.findall(r'"([^"]*)"|(\S+)', body)
    return [(quoted or bare) for quoted, bare in tokens if (quoted or bare)]


def expand(globs: list[str]) -> set[str]:
    out: set[str] = set()
    excluded: set[str] = set()
    for pattern in globs:
        if not isinstance(pattern, str):  # { glob = ..., accel = ... } form
            continue
        if pattern.startswith("!"):
            excluded |= {p.relative_to(ROOT).as_posix() for p in ROOT.glob(pattern[1:])}
            continue
        out |= {p.relative_to(ROOT).as_posix() for p in ROOT.glob(pattern)}
    return out - excluded


def manifest() -> dict:
    with (ROOT / "mcpp.toml").open("rb") as handle:
        return tomllib.load(handle)


def check_core_sources(m: dict) -> None:
    build_cmake = (ROOT / "cmake" / "HuxerUIBuild.cmake").read_text()
    match = re.search(r'file\(GLOB HUXERUI_CORE_SOURCE_FILES.*?"\$\{HUXERUI_PROJECT_DIR\}/(.+?)"',
                      build_cmake, re.S)
    if not match:
        fail("cannot find the core source glob in cmake/HuxerUIBuild.cmake")
        return
    cmake_glob = match.group(1)
    mcpp_globs = m.get("build", {}).get("sources", [])
    if cmake_glob not in mcpp_globs:
        fail(f"core source glob differs: CMake uses '{cmake_glob}', "
             f"mcpp.toml [build] sources is {mcpp_globs}")
        return
    if not expand([cmake_glob]):
        fail(f"core source glob '{cmake_glob}' matches no file")


def target_table(m: dict, selector: str) -> dict:
    return m.get("target", {}).get(selector, {})


def check_platform(m: dict, cmake_file: str, selector: str) -> None:
    text = (ROOT / "cmake" / "platform" / cmake_file).read_text()
    table = target_table(m, selector)

    # --- sources -----------------------------------------------------------
    cmake_sources = {
        s.replace("${HUXERUI_PROJECT_DIR}/", "")
        for s in cmake_list(text, "HUXERUI_PLATFORM_SOURCE_FILES")
    }
    mcpp_sources = expand(table.get("build", {}).get("sources", []))
    if cmake_sources != mcpp_sources:
        only_cmake = sorted(cmake_sources - mcpp_sources)
        only_mcpp = sorted(mcpp_sources - cmake_sources)
        detail = []
        if only_cmake:
            detail.append(f"missing from mcpp.toml [target.'{selector}'.build] sources: "
                          + ", ".join(only_cmake))
        if only_mcpp:
            detail.append(f"missing from cmake/platform/{cmake_file}: " + ", ".join(only_mcpp))
        fail(f"{cmake_file}: platform sources differ -- " + "; ".join(detail))

    # --- link libraries ----------------------------------------------------
    cmake_libs = cmake_list(text, "HUXERUI_PLATFORM_LINK_LIBRARIES")
    if selector == "windows":
        # Windows appends more libraries in an if() branch; take every bare
        # token that looks like a library name anywhere in the file.
        extra = re.findall(r"list\(APPEND HUXERUI_PLATFORM_LINK_LIBRARIES\s+(.+?)\)", text)
        for group in extra:
            cmake_libs += group.split()
        declared = set(table.get("runtime", {}).get("libraries", []))
        missing = {lib for lib in cmake_libs if not lib.startswith("PkgConfig::")} - declared
        if missing:
            fail(f"{cmake_file}: libraries missing from "
                 f"[target.windows.runtime] libraries: {', '.join(sorted(missing))}")
    elif selector == "macos":
        frameworks = {
            lib.replace("-framework ", "").replace("-weak_framework ", "")
            for lib in cmake_libs if "framework" in lib
        }
        weak = {lib.replace("-weak_framework ", "")
                for lib in cmake_libs if lib.startswith("-weak_framework")}
        # frameworks is a TOP-LEVEL [runtime] key: the per-target vocabulary is
        # libraries / link_library_dirs only, so a [target.macos.runtime] entry
        # would be reported and ignored.
        declared = set(m.get("runtime", {}).get("frameworks", []))
        ldflags = " ".join(table.get("build", {}).get("ldflags", []))
        missing = frameworks - weak - declared
        if missing:
            fail(f"{cmake_file}: frameworks missing from [runtime] frameworks: "
                 f"{', '.join(sorted(missing))}")
        for name in sorted(weak):
            if name not in ldflags:
                fail(f"{cmake_file}: weak framework '{name}' missing from "
                     f"[target.macos.build] ldflags (it has no neutral spelling)")
    # Linux: both sides go through pkg-config; nothing to compare.

    # --- compile definitions ----------------------------------------------
    cmake_defines = {
        d for d in cmake_list(text, "HUXERUI_PLATFORM_COMPILE_DEFINITIONS")
        if not d.startswith("$")
    }
    if cmake_defines:
        declared = set(table.get("build", {}).get("defines", []))
        missing = cmake_defines - declared
        if missing:
            fail(f"{cmake_file}: defines missing from [target.'{selector}'.build] "
                 f"defines: {', '.join(sorted(missing))}")


def check_standard(m: dict) -> None:
    root = (ROOT / "CMakeLists.txt").read_text()
    match = re.search(r"set\(CMAKE_CXX_STANDARD (\d+)\)", root)
    if not match:
        fail("cannot find CMAKE_CXX_STANDARD in CMakeLists.txt")
        return
    cmake_standard = f"c++{match.group(1)}"
    mcpp_standard = m.get("package", {}).get("standard")
    if cmake_standard != mcpp_standard:
        fail(f"C++ standard differs: CMakeLists.txt says {cmake_standard}, "
             f"mcpp.toml says {mcpp_standard}")


def check_package_template() -> None:
    """templates/app/ is an mcpp package template; keep it instantiable.

    mcpp renders `**.in` with a closed token vocabulary and copies everything
    else verbatim, so an unknown token is a failure at `mcpp new` time -- on the
    user's machine, not here. The tree is small enough to check exactly.
    """
    root = ROOT / "templates" / "app"
    if not root.is_dir():
        fail("templates/app is missing")
        return
    known = {"project.name", "project.namespace", "project.qualifiedName",
             "template.package.namespace", "template.package.name",
             "template.package.selector", "template.package.version",
             "template.name", "self.name", "self.version"}
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(ROOT)
        text = path.read_text(errors="replace")
        tokens = set(re.findall(r"\{\{([^}]*)\}\}", text))
        if path.suffix != ".in" and tokens:
            fail(f"{rel} carries {{{{...}}}} tokens but is not a .in file, so "
                 f"mcpp copies it verbatim")
        for token in sorted(tokens - known):
            fail(f"{rel} uses unknown template token '{{{{{token}}}}}'")
    meta = root / "template.toml"
    if not meta.is_file():
        fail("templates/app/template.toml is missing")
        return
    with meta.open("rb") as handle:
        declared = tomllib.load(handle).get("template", {})
    if not declared.get("description"):
        fail("templates/app/template.toml declares no description")
    # The composable must not live in the entry file: huxerui.rules leaves the
    # target's entry alone, so a composable there is never transformed.
    entry = root / "src" / "main.cpp.in"
    if entry.is_file() and "[[huxerui::composable]]" in entry.read_text():
        fail("templates/app/src/main.cpp.in marks a composable in the target's "
             "entry, which huxerui.rules never transforms")

    # An mcpp project is module-style: no headers, and the composable lives in a
    # module interface unit.
    headers = [p.relative_to(ROOT) for p in root.rglob("*.h")] + \
              [p.relative_to(ROOT) for p in root.rglob("*.hpp")]
    for header in headers:
        fail(f"{header}: an mcpp project is module-style and should carry no headers")
    # `.in` files are rendered and lose the suffix, so a template's module unit
    # is `app.cppm.in` on disk.
    modules = list(root.rglob("*.cppm")) + list(root.rglob("*.cppm.in"))
    if not modules:
        fail("templates/app declares no module interface unit; the project it "
             "generates would not be module-style")
    for module in modules:
        text = module.read_text()
        # `-include` prepends before `module;`, which is ill-formed, so
        # huxerui.rules does not force includes on a package with module units:
        # the unit carries them in its own global module fragment.
        if "UseState" in text and "#include <typeinfo>" not in text:
            fail(f"{module.relative_to(ROOT)} instantiates typeid through "
                 f"UseState but its global module fragment omits <typeinfo>")


def check_module_shell() -> None:
    """modules/huxerui.cppm is generated; a new public header must reach it."""
    import subprocess
    result = subprocess.run(
        [sys.executable, str(ROOT / "scripts" / "gen_module_exports.py"), "--check"],
        capture_output=True, text=True)
    if result.returncode != 0:
        fail(result.stderr.strip() or "modules/huxerui.cppm is out of date")


def main() -> int:
    m = manifest()
    check_standard(m)
    check_core_sources(m)
    check_module_shell()
    check_package_template()
    for cmake_file, selector in PLATFORMS.items():
        check_platform(m, cmake_file, selector)

    if failures:
        print("mcpp/CMake parity check FAILED:\n", file=sys.stderr)
        for message in failures:
            print(f"  - {message}", file=sys.stderr)
        print("\nBoth build systems describe the same sources and platform "
              "interface; update whichever one is behind.", file=sys.stderr)
        return 1
    print("mcpp/CMake parity check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
