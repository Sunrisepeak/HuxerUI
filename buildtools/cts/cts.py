#!/usr/bin/env python3
"""HuxerUI build-system conformance test suite.

`run` builds every conformance project with one build-system integration, reads the
facts the build-system contract defines from the generated records and the produced
artifact, and checks them against the project's expected facts.
`compare` checks that the integrations which ran agree on the facts that have no fixed
expected value, such as the content of the final resource package.

The expected facts belong to the project, never to an integration: no integration's
output is the reference.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

CTS_ROOT = Path(__file__).resolve().parent
REPOSITORY = CTS_ROOT.parent.parent
PROJECTS = CTS_ROOT / "projects"
ALLOWED_DIFFERENCES = CTS_ROOT / "allowed-differences.json"

# Facts compared between integrations instead of against a fixed expected value.
EQUIVALENCE_FACTS = ("resources.sha256", "resources.files", "libraries.roots")


class Unavailable(Exception):
    """The environment cannot run a check; reported separately from a failure."""


def log(message: str) -> None:
    print(message, flush=True)


def run_command(command: list[str], cwd: Path, env: dict[str, str] | None = None) -> str:
    log(f"  $ {' '.join(command)}  (in {cwd})")
    completed = subprocess.run(command, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if completed.returncode != 0:
        tail = "\n".join(completed.stdout.splitlines()[-60:])
        raise RuntimeError(f"command failed with exit code {completed.returncode}: {' '.join(command)}\n{tail}")
    return completed.stdout


def load_json(path: Path):
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def declaration(integration: str) -> dict:
    return load_json(REPOSITORY / "buildtools" / integration / "support.json")


# ---------------------------------------------------------------- integrations


class CMakeIntegration:
    name = "cmake"

    def __init__(self, arguments: argparse.Namespace) -> None:
        self.home = Path(arguments.cmake_home).resolve() if arguments.cmake_home else REPOSITORY
        self.launcher = arguments.cmake_compiler_launcher

    def build(self, project: Path, work: Path) -> dict:
        graph = work / "libraries.json"
        run_command(
            ["cmake", "-S", str(project), "-B", str(work / "graph"), f"-DHUXERUI_HOME={self.home}",
             "-DHUXERUI_LIBRARY_GRAPH_ONLY=ON", f"-DHUXERUI_LIBRARY_GRAPH_OUTPUT={graph}"],
            project,
        )
        configure = ["cmake", "-S", str(project), "-B", str(work / "build"), "-G", "Ninja",
                     "-DCMAKE_BUILD_TYPE=Debug", f"-DHUXERUI_HOME={self.home}"]
        if self.launcher:
            configure += [f"-DCMAKE_C_COMPILER_LAUNCHER={self.launcher}", f"-DCMAKE_CXX_COMPILER_LAUNCHER={self.launcher}"]
        run_command(configure, project)
        run_command(["cmake", "--build", str(work / "build")], project)
        # A second build of unchanged inputs must succeed without reconfiguring into a failure.
        run_command(["cmake", "--build", str(work / "build")], project)

        plans = sorted((work / "build" / "huxerui-integration").rglob("app.json"))
        if not plans:
            raise RuntimeError("the CMake build generated no application-integration record")
        plan = load_json(plans[0])
        return {
            "target": plan["target"],
            "artifact": plan["artifact"],
            "profile": "debug",
            "libraries": load_json(graph)["libraries"] if graph.exists() else [],
            "launch_environment": {},
        }


class McppIntegration:
    name = "mcpp"

    def __init__(self, arguments: argparse.Namespace) -> None:
        self.toolchain = arguments.mcpp_toolchain

    def build(self, project: Path, work: Path) -> dict:
        command = ["mcpp", "build", "--message-format", "json"]
        if self.toolchain:
            command += ["--toolchain", self.toolchain]
        graph = project / ".huxerui" / "generated" / "libraries.json"
        if graph.exists():
            graph.unlink()
        self._build(command, project)
        envelope = self._build(command, project)

        target = load_json(project / "expected.json")["target"]
        artifacts = [a for a in envelope["data"]["artifacts"] if a["name"] == target and a["kind"] in ("bin", "app")]
        if not artifacts:
            raise RuntimeError(f"mcpp reported no program artifact named {target}")
        profile = envelope["data"]["configuration"]["profile"]
        return {
            "target": target,
            "artifact": artifacts[0]["path"],
            "profile": {"dev": "debug", "release": "release"}.get(profile, profile),
            "libraries": load_json(graph)["libraries"] if graph.exists() else [],
            "launch_environment": (artifacts[0].get("launch") or {}).get("env") or {},
        }

    @staticmethod
    def _build(command: list[str], project: Path) -> dict:
        log(f"  $ {' '.join(command)}  (in {project})")
        completed = subprocess.run(command, cwd=project, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        envelope = None
        for line in completed.stdout.splitlines():
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                document = json.loads(line)
            except json.JSONDecodeError:
                continue
            if document.get("kind") == "mcpp.build":
                envelope = document
        if completed.returncode != 0 or envelope is None or envelope["data"].get("status") != "ok":
            tail = "\n".join((completed.stderr or completed.stdout).splitlines()[-60:])
            raise RuntimeError(f"mcpp build failed with exit code {completed.returncode}\n{tail}")
        return envelope


INTEGRATIONS = {"cmake": CMakeIntegration, "mcpp": McppIntegration}


# ----------------------------------------------------------------------- facts


def needed_libraries(artifact: Path) -> list[str]:
    readelf = shutil.which("readelf") or shutil.which("llvm-readelf")
    if readelf is None:
        raise Unavailable("readelf is not installed")
    output = subprocess.run([readelf, "-d", str(artifact)], text=True, stdout=subprocess.PIPE, check=True).stdout
    return [line.split("[", 1)[1].rstrip("]") for line in output.splitlines() if "(NEEDED)" in line and "[" in line]


def collect_facts(platform: str, project: Path, record: dict) -> dict:
    if platform != "linux":
        raise Unavailable(f"fact collection is not implemented for platform {platform}")
    artifact = Path(record["artifact"])
    facts: dict = {"profile": record["profile"]}
    facts["artifact.name"] = artifact.name if artifact.is_file() else "<missing>"

    needed = needed_libraries(artifact)
    facts["framework.linkage"] = "shared" if any("huxerui" in name for name in needed) else "static"

    package = artifact.with_name(artifact.name + ".resources")
    index = package / "huxerui" / "resources.bin"
    if index.is_file():
        facts["resources.location"] = index.relative_to(artifact.parent).as_posix().replace(artifact.name, "<artifact>")
        facts["resources.sha256"] = hashlib.sha256(index.read_bytes()).hexdigest()
        facts["resources.files"] = sorted(path.relative_to(package).as_posix() for path in package.rglob("*") if path.is_file())
    else:
        facts["resources.location"] = "<missing>"

    facts["libraries"] = [entry["target"] for entry in record["libraries"]]
    facts["libraries.roots"] = [
        os.path.relpath(os.path.realpath(entry["sourceRoot"]), os.path.realpath(project)).replace(os.sep, "/")
        for entry in record["libraries"]
    ]
    return facts


def launch_facts(record: dict, expected: dict, work: Path, timeout: float) -> dict:
    if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        raise Unavailable("no display is available to start the application")
    probe = work / "probe.txt"
    if probe.exists():
        probe.unlink()
    environment = dict(os.environ)
    for key, value in record["launch_environment"].items():
        environment[key] = value
    environment["HUXERUI_CTS_PROBE"] = str(probe)
    log(f"  $ {record['artifact']}  (HUXERUI_CTS_PROBE={probe})")
    output = (work / "launch.log").open("w", encoding="utf-8")
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
            if all(key in observed for key in expected):
                break
            if process.poll() is not None:
                break
            time.sleep(0.2)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
        output.close()
    if not observed:
        tail = "\n".join((work / "launch.log").read_text(encoding="utf-8", errors="replace").splitlines()[-20:])
        raise RuntimeError(f"the application recorded no probe facts before the timeout\n{tail}")
    return {f"launch.{key}": observed.get(key, "<missing>") for key in expected}


# ------------------------------------------------------------------------- run


def allowed_reason(integration: str, project: str, fact: str) -> str | None:
    for entry in load_json(ALLOWED_DIFFERENCES)["differences"]:
        if (fnmatch.fnmatchcase(integration, entry["integration"]) and fnmatch.fnmatchcase(project, entry["project"])
                and fnmatch.fnmatchcase(fact, entry["fact"])):
            return entry["reason"]
    return None


def run_project(integration, support: dict, platform: str, project: Path, out: Path, arguments) -> dict:
    expected = load_json(project / "expected.json")
    result: dict = {"project": project.name, "integration": integration.name, "platform": platform,
                    "status": "pass", "facts": {}, "mismatches": [], "allowed": [], "unavailable": []}
    platform_support = support["platforms"].get(platform)
    missing = [capability for capability in expected.get("requires", [])
               if platform_support is None or capability not in platform_support.get("capabilities", [])]
    if platform_support is None or missing:
        result["status"] = "undeclared"
        result["reason"] = (f"platform {platform} is not declared" if platform_support is None
                            else "capabilities not declared: " + ", ".join(missing))
        return result

    work = out / "work" / integration.name / project.name
    work.mkdir(parents=True, exist_ok=True)
    try:
        record = integration.build(project, work)
        facts = collect_facts(platform, project, record)
        if arguments.launch and expected.get("launch"):
            try:
                facts.update(launch_facts(record, expected["launch"], work, arguments.launch_timeout))
            except Unavailable as unavailable:
                result["unavailable"].append(f"launch: {unavailable}")
        result["facts"] = facts
    except Unavailable as unavailable:
        result["status"] = "unavailable"
        result["reason"] = str(unavailable)
        return result
    except RuntimeError as failure:
        result["status"] = "fail"
        result["reason"] = str(failure)
        return result

    wanted = dict(expected["facts"])
    if arguments.launch and not result["unavailable"]:
        wanted.update({f"launch.{key}": value for key, value in expected.get("launch", {}).items()})
    for fact, value in wanted.items():
        actual = result["facts"].get(fact, "<absent>")
        if actual == value:
            continue
        reason = allowed_reason(integration.name, project.name, fact)
        entry = {"fact": fact, "expected": value, "actual": actual}
        if reason:
            result["allowed"].append({**entry, "reason": reason})
        else:
            result["mismatches"].append(entry)
    if result["mismatches"]:
        result["status"] = "fail"
    return result


def command_run(arguments) -> int:
    integration = INTEGRATIONS[arguments.integration](arguments)
    support = declaration(arguments.integration)
    out = Path(arguments.out).resolve()
    results_directory = out / "results" / arguments.integration
    results_directory.mkdir(parents=True, exist_ok=True)
    projects = sorted(path for path in PROJECTS.iterdir() if (path / "expected.json").is_file())
    if arguments.project:
        projects = [path for path in projects if path.name in arguments.project]
    failed = False
    for project in projects:
        log(f"== {arguments.integration} / {arguments.platform} / {project.name}")
        result = run_project(integration, support, arguments.platform, project, out, arguments)
        (results_directory / f"{project.name}.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        log(f"   {result['status'].upper()}" + (f": {result['reason']}" if result.get("reason") else ""))
        for mismatch in result["mismatches"]:
            log(f"   DIFFERS  {mismatch['fact']}\n            expected: {mismatch['expected']}\n            actual:   {mismatch['actual']}")
        for allowed in result["allowed"]:
            log(f"   allowed  {allowed['fact']}: {allowed['reason']}")
        for unavailable in result["unavailable"]:
            log(f"   unavailable  {unavailable}")
        failed = failed or result["status"] == "fail"
    return 1 if failed else 0


# --------------------------------------------------------------------- compare


def command_compare(arguments) -> int:
    results: dict[str, dict[str, dict]] = {}
    for directory in arguments.results:
        for path in sorted(Path(directory).rglob("*.json")):
            result = load_json(path)
            results.setdefault(result["project"], {})[result["integration"]] = result
    if not results:
        log("no conformance results were found")
        return 1

    integrations = sorted({name for by_integration in results.values() for name in by_integration})
    failed = False
    log("project".ljust(20) + "".join(name.ljust(14) for name in integrations))
    for project in sorted(results):
        row = project.ljust(20)
        for name in integrations:
            result = results[project].get(name)
            row += (result["status"] if result else "not run").ljust(14)
            failed = failed or (result is not None and result["status"] == "fail")
        log(row)

    for project in sorted(results):
        ran = {name: result for name, result in results[project].items() if result["status"] == "pass"}
        if len(ran) < 2:
            continue
        for fact in EQUIVALENCE_FACTS:
            values = {name: result["facts"].get(fact, "<absent>") for name, result in ran.items()}
            if len({json.dumps(value, sort_keys=True) for value in values.values()}) == 1:
                log(f"equal    {project}  {fact}")
                continue
            reason = allowed_reason("*", project, fact)
            if reason:
                log(f"allowed  {project}  {fact}: {reason}")
                continue
            failed = True
            log(f"DIFFERS  {project}  {fact}")
            for name, value in values.items():
                log(f"           {name}: {value}")
    # A project may state that another project is the same program in a different source form,
    # such as C++ modules instead of headers; the form must not change the program model.
    for project in sorted(results):
        twin = load_json(PROJECTS / project / "expected.json").get("sameModelAs")
        if not twin:
            continue
        for name, result in results[project].items():
            other = results.get(twin, {}).get(name)
            if result["status"] != "pass" or other is None or other["status"] != "pass":
                continue
            for fact in EQUIVALENCE_FACTS:
                if result["facts"].get(fact) == other["facts"].get(fact):
                    log(f"equal    {project} = {twin}  {fact}  ({name})")
                else:
                    failed = True
                    log(f"DIFFERS  {project} vs {twin}  {fact}  ({name})\n"
                        f"           {project}: {result['facts'].get(fact)}\n           {twin}: {other['facts'].get(fact)}")
    log("conformance FAILED" if failed else "conformance passed")
    return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    run = commands.add_parser("run", help="build the conformance projects with one integration and check their facts")
    run.add_argument("--integration", required=True, choices=sorted(INTEGRATIONS))
    run.add_argument("--platform", required=True)
    run.add_argument("--out", required=True, help="directory for build output and result files")
    run.add_argument("--project", action="append", help="limit the run to the named project; repeatable")
    run.add_argument("--launch", action="store_true", help="start each application and check what it resolved")
    run.add_argument("--launch-timeout", type=float, default=30.0)
    run.add_argument("--cmake-home", help="HuxerUI SDK or source checkout for CMake projects; defaults to this checkout")
    run.add_argument("--cmake-compiler-launcher", help="compiler launcher such as ccache for CMake builds")
    run.add_argument("--mcpp-toolchain", help="toolchain passed to mcpp, for example llvm@22.1.8")
    run.set_defaults(handler=command_run)

    compare = commands.add_parser("compare", help="check that the integrations which ran agree with each other")
    compare.add_argument("results", nargs="+", help="result directories written by run")
    compare.set_defaults(handler=command_compare)

    arguments = parser.parse_args()
    return arguments.handler(arguments)


if __name__ == "__main__":
    sys.exit(main())
