#!/usr/bin/env bash
# compare_program_model.sh <specification> <CMake facts> <mcpp facts>
#
# Compares the program model facts of the same project built by CMake and by
# mcpp. A fact whose values differ, or which one side lacks, fails the check
# unless the specification's allowed-differences block names it; the block is
# read from docs/design/build-systems-spec.md, so the list exists once.
set -euo pipefail

spec=${1:?usage: compare_program_model.sh <specification> <CMake facts> <mcpp facts>}
cmake_facts=${2:?usage: compare_program_model.sh <specification> <CMake facts> <mcpp facts>}
mcpp_facts=${3:?usage: compare_program_model.sh <specification> <CMake facts> <mcpp facts>}

python3 - "$spec" "$cmake_facts" "$mcpp_facts" <<'PY'
import fnmatch, sys

spec, cmake_path, mcpp_path = sys.argv[1:4]

allowed = []
inside = False
for line in open(spec, encoding="utf-8"):
    if "<!-- allowed-differences:begin -->" in line:
        inside = True
        continue
    if "<!-- allowed-differences:end -->" in line:
        break
    if not inside or line.startswith("```") or not line.strip():
        continue
    pattern, _, reason = line.strip().partition(" ")
    allowed.append((pattern, reason.strip()))
if not allowed:
    sys.exit(f"{spec} has no allowed-differences block")

def read(path):
    facts = {}
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line:
            continue
        key, _, value = line.partition("=")
        facts[key] = value
    return facts

cmake, mcpp = read(cmake_path), read(mcpp_path)
failures, tolerated = [], []
for key in sorted(set(cmake) | set(mcpp)):
    left, right = cmake.get(key, "<absent>"), mcpp.get(key, "<absent>")
    if left == right:
        continue
    reason = next((r for p, r in allowed if fnmatch.fnmatchcase(key, p)), None)
    (tolerated if reason else failures).append((key, left, right, reason))

print(f"{len(cmake)} CMake facts, {len(mcpp)} mcpp facts")
for key, left, right, reason in tolerated:
    print(f"allowed  {key}\n         CMake: {left}\n         mcpp:  {right}\n         why:   {reason}")
for key, left, right, _ in failures:
    print(f"DIFFERS  {key}\n         CMake: {left}\n         mcpp:  {right}")
if failures:
    print(f"\n{len(failures)} fact(s) differ and docs/design/build-systems-spec.md allows none of them")
    sys.exit(1)
print("the default program models agree, apart from the differences the specification allows")
PY
