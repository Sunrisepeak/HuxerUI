#!/usr/bin/env python3
"""HuxerUI build-system conformance test suite.

The suite is organized as checkpoints. `build` builds every conformance project with one
build-system integration and records the facts the build-system contract defines.
`check` evaluates one or more checkpoints against each project's expected facts.
`compare` checks that the integrations which ran agree on the facts that have no fixed
expected value, such as the content of the final resource package.

An integration is a directory `buildtools/<integration>/` with a `support.json`; the runner
discovers it there and builds through the adapter beside it, `cts_adapter.py` (see
`adapter_api.py`). An integration is checked for what it declares in its `support.json`: a
target, host, checkpoint, or capability it does not declare is reported as undeclared and
skipped, never failed.

The expected facts belong to the project, never to an integration: no integration's
output is the reference.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import importlib.util
import json
import os
import platform as host_platform
import shutil
import subprocess
import sys
import time
from pathlib import Path

CTS_ROOT = Path(__file__).resolve().parent
REPOSITORY = CTS_ROOT.parent.parent
BUILDTOOLS = REPOSITORY / "buildtools"
PROJECTS = CTS_ROOT / "projects"
ALLOWED_DIFFERENCES = CTS_ROOT / "allowed-differences.json"

# Each checkpoint names the facts checked against a project's expected.json and the facts
# that integrations, and projects related by `sameModelAs`, must agree on.
CHECKS: dict[str, dict] = {
    "build": {
        "title": "Default build: profile and artifact name",
        "facts": ["profile", "artifact.name", "artifact.bundle"],
        "equivalent": [],
    },
    "linkage": {
        "title": "Framework linkage",
        "facts": ["framework.linkage", "android.exports", "ios.link_inputs"],
        "equivalent": [],
    },
    "resources": {
        "title": "Final resource package and its runtime location",
        "facts": ["resources.location"],
        "equivalent": ["resources.sha256", "resources.files"],
    },
    "libraries": {
        "title": "Ordered library-graph record",
        "facts": ["libraries"],
        "equivalent": ["libraries.roots"],
    },
    "launch": {
        "title": "Startup and resource resolution",
        "facts": [],
        "equivalent": [],
    },
}


sys.path.insert(0, str(CTS_ROOT))
from adapter_api import Unavailable, load_json, log, validate_record  # noqa: E402


def current_host() -> str:
    return {"Linux": "linux", "Darwin": "macos", "Windows": "windows"}.get(host_platform.system(), host_platform.system().lower())


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def target_support(integration: str, target: str, host: str) -> tuple[dict | None, str]:
    declaration = load_json(BUILDTOOLS / integration / "support.json")
    support = declaration["targets"].get(target)
    if support is None:
        return None, f"target {target} is not declared"
    if host not in support["hosts"]:
        return None, f"host {host} is not declared for target {target}"
    return support, ""


def expected_for(project: Path, target: str) -> dict:
    expected = load_json(project / "expected.json")
    override = expected.get("targets", {}).get(target, {})
    expected["facts"] = {**expected["facts"], **override.get("facts", {})}
    return expected


def selected_projects(names: list[str] | None) -> list[Path]:
    projects = sorted(path for path in PROJECTS.iterdir() if (path / "expected.json").is_file())
    return [path for path in projects if path.name in names] if names else projects


def result_path(out: Path, integration: str, target: str, project: str) -> Path:
    return out / "results" / f"{integration}-{target}-on-{current_host()}" / f"{project}.json"


# ---------------------------------------------------------------- integrations


def discover_integrations() -> dict[str, Path]:
    """Every directory below `buildtools` that declares support is an integration."""
    return {path.parent.name: path.parent for path in sorted(BUILDTOOLS.glob("*/support.json"))}


def load_adapter(name: str, directory: Path):
    """Imports an integration's adapter module, or returns None when it ships none."""
    path = directory / "cts_adapter.py"
    if not path.is_file():
        return None
    spec = importlib.util.spec_from_file_location(f"cts_adapter_{name}", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    for required in ("add_arguments", "Adapter"):
        if not hasattr(module, required):
            raise SystemExit(f"{path}: an adapter defines {required}")
    return module


def load_adapters() -> dict[str, object]:
    adapters = {}
    for name, directory in discover_integrations().items():
        module = load_adapter(name, directory)
        if module is not None:
            adapters[name] = module
    return adapters


class PrefixedOptions:
    """Adds an adapter's options and refuses an option that does not carry the integration's name."""

    def __init__(self, parser, name: str) -> None:
        self.parser = parser
        self.prefix = f"--{name}-"

    def add_argument(self, *flags, **options):
        for flag in flags:
            if not flag.startswith(self.prefix):
                raise SystemExit(f"adapter option {flag} does not start with {self.prefix}")
        return self.parser.add_argument(*flags, **options)


ADAPTERS = load_adapters()


# ----------------------------------------------------------------------- facts


def dynamic_dependencies(target: str, artifact: Path) -> list[str]:
    if target == "linux":
        readelf = shutil.which("readelf") or shutil.which("llvm-readelf")
        if readelf is None:
            raise Unavailable("readelf is not installed")
        output = subprocess.run([readelf, "-d", str(artifact)], text=True, stdout=subprocess.PIPE, check=True).stdout
        return [line.split("[", 1)[1].rstrip("]") for line in output.splitlines() if "(NEEDED)" in line and "[" in line]
    if target == "macos":
        output = subprocess.run(["otool", "-L", str(artifact)], text=True, stdout=subprocess.PIPE, check=True).stdout
        return [line.split()[0] for line in output.splitlines()[1:] if line.strip()]
    if target == "windows":
        # A PE import table spells each imported library's file name as ASCII.
        data = artifact.read_bytes().lower()
        return ["huxerui.dll"] if b"huxerui.dll" in data else []
    raise Unavailable(f"dynamic dependencies cannot be read for target {target}")


def collect_facts(target: str, project: Path, record: dict) -> dict:
    if target not in ("linux", "windows", "macos", "web", "android", "ios"):
        raise Unavailable(f"fact collection is not implemented for target {target}")
    artifact = Path(record["artifact"])
    facts: dict = {"profile": record["profile"]}
    if target == "web":
        return {**facts, **web_facts(artifact), **library_facts(project, record)}
    if target == "android":
        package = Path(record["resources"]) if record.get("resources") else None
        return {**facts, **android_facts(artifact, package), **library_facts(project, record)}
    if target == "ios":
        return {**facts, **ios_facts(artifact, record), **library_facts(project, record)}
    # The contract fixes the basename; the platform adds its own extension.
    name = artifact.name[:-4] if target == "windows" and artifact.name.lower().endswith(".exe") else artifact.name
    facts["artifact.name"] = name if artifact.is_file() else "<missing>"
    bundle = Path(record["bundle"]) if record.get("bundle") else None
    facts["artifact.bundle"] = bundle.name if bundle is not None and bundle.is_dir() else "<none>"

    needed = dynamic_dependencies(target, artifact)
    facts["framework.linkage"] = "shared" if any("huxerui" in Path(entry).name.lower() for entry in needed) else "static"

    if target == "macos":
        # The Runtime reads `HuxerUI` in the main bundle's resources, which is the program's
        # own directory when the program is not in a bundle.
        package = (bundle / "Contents" / "Resources" / "HuxerUI") if bundle is not None else artifact.parent / "HuxerUI"
        location = "<bundle>/Contents/Resources/HuxerUI/huxerui/resources.bin" if bundle is not None \
            else "<artifact-directory>/HuxerUI/huxerui/resources.bin"
    else:
        # The Runtime replaces the program's extension with `.resources`.
        package = artifact.with_suffix(".resources")
        location = "<artifact>.resources/huxerui/resources.bin"
    index = package / "huxerui" / "resources.bin"
    if index.is_file():
        facts["resources.location"] = location
        facts["resources.sha256"] = hashlib.sha256(index.read_bytes()).hexdigest()
        facts["resources.files"] = sorted(path.relative_to(package).as_posix() for path in package.rglob("*") if path.is_file())
    else:
        facts["resources.location"] = "<missing>"

    facts.update(library_facts(project, record))
    return facts


def library_facts(project: Path, record: dict) -> dict:
    return {
        "libraries": [entry["target"] for entry in record["libraries"]],
        "libraries.roots": [
            os.path.relpath(os.path.realpath(entry["sourceRoot"]), os.path.realpath(project)).replace(os.sep, "/")
            for entry in record["libraries"]
        ],
    }


def find_readelf() -> str:
    """An ELF reader for a cross-built artifact: the host's, or the one an installed Android NDK carries.
    Adapters may name the directories where their integration installs such an NDK."""
    found = shutil.which("readelf") or shutil.which("llvm-readelf")
    if found:
        return found
    roots = [os.environ.get(name) for name in ("ANDROID_NDK_HOME", "ANDROID_NDK_ROOT", "ANDROID_NDK_LATEST_HOME")]
    for module in ADAPTERS.values():
        roots += [str(path) for path in getattr(module, "tool_search_roots", lambda: [])()]
    for root in filter(None, roots):
        for candidate in Path(root).glob("toolchains/llvm/prebuilt/*/bin/llvm-readelf*"):
            if candidate.is_file():
                return str(candidate)
    raise Unavailable("no ELF reader was found on this host")


def android_facts(artifact: Path, package: Path | None) -> dict:
    """Facts of an Android application core: the application's shared library and the final resource package
    the platform shell stages into the APK."""
    facts: dict = {"artifact.bundle": "<none>"}
    facts["artifact.name"] = artifact.name if artifact.is_file() else "<missing>"
    readelf = find_readelf()
    dynamic = subprocess.run([readelf, "-d", str(artifact)], text=True, stdout=subprocess.PIPE, check=True).stdout
    needed = [line.split("[", 1)[1].rstrip("]") for line in dynamic.splitlines() if "(NEEDED)" in line and "[" in line]
    facts["framework.linkage"] = "shared" if any("huxerui" in name for name in needed) else "static"

    # The platform finds JNI entries by name: they must be exported, and nothing else may be.
    symbols = subprocess.run([readelf, "--dyn-syms", "-W", str(artifact)], text=True, stdout=subprocess.PIPE, check=True).stdout
    exported = [fields[7] for fields in (line.split() for line in symbols.splitlines())
                if len(fields) >= 8 and fields[4] in ("GLOBAL", "WEAK") and fields[6] != "UND"]
    jni = [name for name in exported if name.startswith("Java_") or name.startswith("JNI_On")]
    if not jni:
        facts["android.exports"] = "no JNI entries"
    elif len(jni) != len(exported):
        facts["android.exports"] = "JNI entries and other symbols"
    else:
        facts["android.exports"] = "JNI entries only"

    facts["resources.location"] = "<missing>"
    if package is not None and (package / "huxerui" / "resources.bin").is_file():
        facts["resources.location"] = "application core"
        facts["resources.sha256"] = hashlib.sha256((package / "huxerui" / "resources.bin").read_bytes()).hexdigest()
        facts["resources.files"] = sorted(path.relative_to(package).as_posix() for path in package.rglob("*") if path.is_file())
    return facts


def ios_facts(artifact: Path, record: dict) -> dict:
    """Facts of an iOS application core: the static archive the Xcode shell links, its link inputs,
    and the final resource package the shell stages into the bundle."""
    facts: dict = {"artifact.bundle": "<none>"}
    facts["artifact.name"] = artifact.name if artifact.is_file() else "<missing>"
    facts["framework.linkage"] = "<missing>"
    if artifact.is_file():
        members = subprocess.run(["ar", "-t", str(artifact)], text=True, stdout=subprocess.PIPE, check=True).stdout
        # The framework is inside the core when the archive carries its UIKit platform objects.
        facts["framework.linkage"] = "static" if "uikit" in members.lower() else "not in the application core"
    link_inputs = Path(record.get("link_inputs") or "")
    facts["ios.link_inputs"] = "present" if link_inputs.is_file() and link_inputs.stat().st_size > 0 else "<missing>"

    package = Path(record["resources"]) if record.get("resources") else None
    index = package / "huxerui" / "resources.bin" if package is not None else None
    facts["resources.location"] = "<missing>"
    if index is not None and index.is_file():
        facts["resources.location"] = "application core"
        facts["resources.sha256"] = hashlib.sha256(index.read_bytes()).hexdigest()
        facts["resources.files"] = sorted(path.relative_to(package).as_posix() for path in package.rglob("*") if path.is_file())
    return facts


def web_facts(artifact: Path) -> dict:
    """Facts of a Web application: its ES module, WebAssembly module, and preloaded resource package."""
    facts: dict = {"artifact.bundle": "<none>", "framework.linkage": "static"}
    name = artifact.name[:-3] if artifact.name.endswith(".js") else artifact.name
    wasm = artifact.with_suffix(".wasm")
    facts["artifact.name"] = name if artifact.is_file() and wasm.is_file() else "<missing>"

    # Emscripten's loader lists each preloaded file with its byte range in the data package.
    data = artifact.with_suffix(".data")
    facts["resources.location"] = "<missing>"
    if not data.is_file():
        return facts
    import re
    loader = artifact.read_text(encoding="utf-8", errors="replace")
    ranges = {}
    for match in re.finditer(r'\{"filename":\s*"([^"]+)",\s*"start":\s*(\d+),\s*"end":\s*(\d+)', loader):
        ranges[match.group(1)] = (int(match.group(2)), int(match.group(3)))
    index = ranges.get("/huxerui/resources.bin")
    if index is None:
        return facts
    content = data.read_bytes()
    facts["resources.location"] = "<artifact>.data:/huxerui/resources.bin"
    facts["resources.sha256"] = hashlib.sha256(content[index[0]:index[1]]).hexdigest()
    facts["resources.files"] = sorted(path.lstrip("/") for path in ranges)
    return facts


def launch_facts(record: dict, expected: dict, work: Path, timeout: float) -> dict:
    if current_host() == "linux" and not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        raise Unavailable("no display is available to start the application")
    probe = work / "probe.txt"
    if probe.exists():
        probe.unlink()
    environment = dict(os.environ)
    environment.update(record["launch_environment"])
    environment["HUXERUI_CTS_PROBE"] = str(probe)
    log(f"  $ {record['artifact']}  (HUXERUI_CTS_PROBE={probe})")
    with (work / "launch.log").open("w", encoding="utf-8") as output:
        process = subprocess.Popen([record["artifact"]], cwd=Path(record["artifact"]).parent, env=environment,
                                   stdout=output, stderr=subprocess.STDOUT)
        observed: dict[str, str] = {}
        deadline = time.monotonic() + timeout
        try:
            while time.monotonic() < deadline:
                if probe.exists():
                    for line in probe.read_text(encoding="utf-8").splitlines():
                        key, separator, value = line.partition("=")
                        if separator:
                            observed[key] = value
                if all(key in observed for key in expected) or process.poll() is not None:
                    break
                time.sleep(0.2)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
    if not observed:
        tail = "\n".join((work / "launch.log").read_text(encoding="utf-8", errors="replace").splitlines()[-20:])
        raise RuntimeError(f"the application recorded no probe facts before the timeout\n{tail}")
    return {f"launch.{key}": observed.get(key, "<missing>") for key in expected}


# ----------------------------------------------------------------------- build


def build_project(name: str, adapter, target: str, host: str, project: Path, out: Path) -> dict:
    expected = expected_for(project, target)
    result: dict = {"project": project.name, "integration": name, "target": target, "host": host,
                    "status": "built", "reason": "", "record": {}, "facts": {}}
    support, reason = target_support(name, target, host)
    if support is None:
        result.update(status="undeclared", reason=reason)
        return result
    missing = [capability for capability in expected.get("requires", []) if capability not in support["capabilities"]]
    if missing:
        result.update(status="undeclared", reason="capabilities not declared: " + ", ".join(missing))
        return result

    work = out / "work" / f"{name}-{target}" / project.name
    work.mkdir(parents=True, exist_ok=True)
    try:
        validate_record(name, adapter.build(project, work, target))
        # A second build of unchanged inputs must also succeed; its record is the one checked.
        try:
            result["record"] = validate_record(name, adapter.build(project, work, target))
        except RuntimeError as failure:
            raise RuntimeError(f"the second build of unchanged inputs failed: {failure}") from failure
        result["facts"] = {**collect_facts(target, project, result["record"]), "rebuild": "succeeded"}
    except Unavailable as unavailable:
        result.update(status="unavailable", reason=str(unavailable))
    except RuntimeError as failure:
        result.update(status="failed", reason=str(failure))
    return result


def command_build(arguments) -> int:
    adapter = ADAPTERS[arguments.integration].Adapter(arguments, REPOSITORY)
    out = Path(arguments.out).resolve()
    failed = False
    for project in selected_projects(arguments.project):
        log(f"== build  {arguments.integration} / {arguments.target} / {project.name}")
        result = build_project(arguments.integration, adapter, arguments.target, arguments.host, project, out)
        write_json(result_path(out, arguments.integration, arguments.target, project.name), result)
        log(f"   {result['status'].upper()}" + (f": {result['reason']}" if result["reason"] else ""))
        failed = failed or result["status"] == "failed"
    return 1 if failed else 0


# ----------------------------------------------------------------------- check


def allowed_reason(integration: str, project: str, fact: str) -> str | None:
    for entry in load_json(ALLOWED_DIFFERENCES)["differences"]:
        if (fnmatch.fnmatchcase(integration, entry["integration"]) and fnmatch.fnmatchcase(project, entry["project"])
                and fnmatch.fnmatchcase(fact, entry["fact"])):
            return entry["reason"]
    return None


def summarize(title: str, rows: list[tuple[str, str, str]]) -> None:
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary:
        return
    with open(summary, "a", encoding="utf-8") as stream:
        stream.write(f"### {title}\n\n| Project | Result | Detail |\n| --- | --- | --- |\n")
        for project, status, detail in rows:
            stream.write(f"| `{project}` | {status} | {detail.splitlines()[0] if detail else ''} |\n")
        stream.write("\n")


def check_project(check: str, integration: str, target: str, host: str, project: Path, out: Path, arguments) -> tuple[str, str]:
    path = result_path(out, integration, target, project.name)
    if not path.is_file():
        return "fail", "the project was not built; run `build` first"
    result = load_json(path)
    if result["status"] == "undeclared":
        return "undeclared", result["reason"]
    support, reason = target_support(integration, target, host)
    if support is None:
        return "undeclared", reason
    if check not in support["checks"]:
        return "undeclared", f"checkpoint {check} is not declared"
    if result["status"] == "unavailable":
        return "unavailable", result["reason"]
    if result["status"] != "built":
        return "fail", "the build failed: " + result["reason"]

    expected = expected_for(project, target)
    wanted = {fact: expected["facts"][fact] for fact in CHECKS[check]["facts"] if fact in expected["facts"]}
    if check == "launch":
        launch = expected.get("launch", {})
        if not launch:
            return "pass", "the project defines no launch facts"
        work = out / "work" / f"{integration}-{target}" / project.name
        try:
            result["facts"].update(launch_facts(result["record"], launch, work, arguments.launch_timeout))
        except Unavailable as unavailable:
            return "unavailable", str(unavailable)
        except RuntimeError as failure:
            return "fail", str(failure)
        write_json(path, result)
        wanted = {f"launch.{key}": value for key, value in launch.items()}

    problems, notes = [], []
    for fact, value in wanted.items():
        actual = result["facts"].get(fact, "<absent>")
        if actual == value:
            continue
        reason = allowed_reason(integration, project.name, fact)
        if reason:
            notes.append(f"allowed {fact}: {reason}")
        else:
            problems.append(f"{fact}: expected {json.dumps(value)}, actual {json.dumps(actual)}")
    if problems:
        return "fail", "; ".join(problems)
    return "pass", "; ".join(notes)


def command_check(arguments) -> int:
    out = Path(arguments.out).resolve()
    checks = list(CHECKS) if arguments.check == ["all"] else arguments.check
    failed = False
    for check in checks:
        log(f"== check  {check}: {CHECKS[check]['title']}  ({arguments.integration} / {arguments.target})")
        rows = []
        for project in selected_projects(arguments.project):
            status, detail = check_project(check, arguments.integration, arguments.target, arguments.host, project, out, arguments)
            rows.append((project.name, status, detail))
            log(f"   {status.upper():<12}{project.name}" + (f"\n                {detail}" if detail else ""))
            failed = failed or status == "fail"
        summarize(f"{check}: {CHECKS[check]['title']} ({arguments.integration}, {arguments.target})", rows)
    return 1 if failed else 0


def command_run(arguments) -> int:
    built = command_build(arguments)
    arguments.check = ["all"] if arguments.check is None else arguments.check
    checked = command_check(arguments)
    return 1 if built or checked else 0


# --------------------------------------------------------------------- compare


def command_compare(arguments) -> int:
    # results[target][project][integration]
    results: dict[str, dict[str, dict[str, dict]]] = {}
    for directory in arguments.results:
        for path in sorted(Path(directory).rglob("*.json")):
            result = load_json(path)
            if "integration" not in result:
                continue
            # Builds are compared within one host: a host's checkout and tools are part of what produced them.
            group = f"{result['target']} on {result.get('host', 'unknown')}"
            results.setdefault(group, {}).setdefault(result["project"], {})[result["integration"]] = result
    if not results:
        log("no conformance results were found")
        return 1

    checks = [name for name in CHECKS if CHECKS[name]["equivalent"]] if arguments.check in (None, ["all"]) else arguments.check
    failed = False
    for check in checks:
        facts = CHECKS[check]["equivalent"]
        if not facts:
            log(f"== compare  {check}: nothing to compare between integrations")
            continue
        rows = []
        for target in sorted(results):
            log(f"== compare  {check}: {CHECKS[check]['title']}  (target {target})")
            for project in sorted(results[target]):
                built = {name: result for name, result in results[target][project].items() if result["status"] == "built"}
                groups = []
                if len(built) >= 2:
                    groups.append((project, "between " + ", ".join(sorted(built)),
                                   {name: result["facts"] for name, result in built.items()}))
                # A project may state that another project is the same program in a different
                # source form, such as C++ modules; the form must not change the program model.
                twin = load_json(PROJECTS / project / "expected.json").get("sameModelAs")
                for name, result in built.items():
                    other = results[target].get(twin, {}).get(name) if twin else None
                    if other is not None and other["status"] == "built":
                        groups.append((project, f"same model as {twin} ({name})",
                                       {project: result["facts"], twin: other["facts"]}))
                if not groups:
                    rows.append((project, "skipped", "fewer than two integrations built it"))
                    log(f"   SKIPPED     {project}: fewer than two integrations built it")
                for name, label, by_source in groups:
                    differing = []
                    for fact in facts:
                        values = {source: found.get(fact, "<absent>") for source, found in by_source.items()}
                        if len({json.dumps(value, sort_keys=True) for value in values.values()}) > 1 \
                                and not allowed_reason("*", project, fact):
                            differing.append(f"{fact}: " + ", ".join(f"{source}={json.dumps(value)}" for source, value in values.items()))
                    status = "fail" if differing else "pass"
                    rows.append((name, status, label + ("; " + "; ".join(differing) if differing else "")))
                    log(f"   {status.upper():<12}{name}: {label}" + ("\n                " + "\n                ".join(differing) if differing else ""))
                    failed = failed or bool(differing)
        summarize(f"compare {check}: {CHECKS[check]['title']}", rows)
    return 1 if failed else 0


def command_matrix(arguments) -> int:
    """Prints the status of every built project as one table."""
    rows: dict[str, dict[str, str]] = {}
    for directory in arguments.results:
        for path in sorted(Path(directory).rglob("*.json")):
            result = load_json(path)
            if "integration" in result:
                column = f"{result['integration']}/{result['target']}@{result.get('host', '?')}"
                rows.setdefault(result["project"], {})[column] = result["status"]
    columns = sorted({column for row in rows.values() for column in row})
    for column in columns:
        log(f"{column:<28}" + "  ".join(f"{project}={rows[project].get(column, 'not run')}" for project in sorted(rows)))
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as stream:
            stream.write("### Build matrix\n\n| Project | " + " | ".join(columns) + " |\n| --- |" + " --- |" * len(columns) + "\n")
            for project in sorted(rows):
                stream.write(f"| `{project}` | " + " | ".join(rows[project].get(column, "not run") for column in columns) + " |\n")
    return 0


def command_list(arguments) -> int:
    for name, directory in discover_integrations().items():
        targets = sorted(load_json(directory / "support.json")["targets"])
        state = "" if name in ADAPTERS else "  (no cts_adapter.py; not buildable by the suite)"
        log(f"integration  {name}: {', '.join(targets)}{state}")
    for name, check in CHECKS.items():
        log(f"check    {name}: {check['title']}")
    for project in selected_projects(None):
        requires = load_json(project / "expected.json").get("requires", [])
        log(f"project  {project.name}" + (f"  (requires {', '.join(requires)})" if requires else ""))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    def add_selection(command, with_checks: bool) -> None:
        command.add_argument("--integration", required=True, choices=sorted(ADAPTERS))
        command.add_argument("--target", required=True, help="target platform, for example linux")
        command.add_argument("--host", default=current_host(), help="host platform; defaults to this machine")
        command.add_argument("--out", required=True, help="directory for build output and result files")
        command.add_argument("--project", action="append", help="limit to the named project; repeatable")
        for name, module in sorted(ADAPTERS.items()):
            module.add_arguments(PrefixedOptions(command.add_argument_group(f"{name} integration"), name))
        if with_checks:
            command.add_argument("--check", nargs="+", choices=[*CHECKS, "all"], help="checkpoints to evaluate")
            command.add_argument("--launch-timeout", type=float, default=30.0)

    build = commands.add_parser("build", help="build the conformance projects and record their facts")
    add_selection(build, with_checks=False)
    build.set_defaults(handler=command_build)

    check = commands.add_parser("check", help="evaluate checkpoints for projects that were built")
    add_selection(check, with_checks=True)
    check.set_defaults(handler=command_check)

    run = commands.add_parser("run", help="build, then evaluate the selected checkpoints (all by default)")
    add_selection(run, with_checks=True)
    run.set_defaults(handler=command_run)

    compare = commands.add_parser("compare", help="check that integrations agree on a checkpoint's facts")
    compare.add_argument("--check", action="append", choices=[*CHECKS, "all"],
                         help="checkpoint to compare; repeatable; all comparable checkpoints by default")
    compare.add_argument("results", nargs="+", help="result directories written by build")
    compare.set_defaults(handler=command_compare)

    matrix = commands.add_parser("matrix", help="print the build status of every project and integration")
    matrix.add_argument("results", nargs="+")
    matrix.set_defaults(handler=command_matrix)

    listing = commands.add_parser("list", help="list the checkpoints and projects")
    listing.set_defaults(handler=command_list)

    arguments = parser.parse_args()
    if arguments.command == "check" and arguments.check is None:
        arguments.check = ["all"]
    return arguments.handler(arguments) or 0


if __name__ == "__main__":
    sys.exit(main())
