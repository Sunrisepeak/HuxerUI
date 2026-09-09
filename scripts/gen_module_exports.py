#!/usr/bin/env python3
"""Generate modules/huxerui.cppm -- the C++20 module shell over the public headers.

HuxerUI's public API stays in `include/huxerui/*.h`: those headers are the SDK
contract, they are what CMake installs, and `#include <huxerui/huxerui.h>` keeps
working unchanged. The module is an ADDITIONAL front door, built by including
those headers in the global module fragment and re-exporting their
namespace-scope names:

    module;
    #include <huxerui/huxerui.h>
    export module huxerui;
    export namespace huxerui { using huxerui::View; ... }

Entities declared in the global module fragment are attached to the global
module, so the module changes no linkage, no mangling and no ABI. That is what
makes `import huxerui;` link against exactly the same library `#include` does,
and it is why no BMI is ever shipped: a BMI is bound to one compiler and one
standard level, while a .cppm is bound to neither.

The list has to be generated -- roughly a thousand declarations across 55
headers -- but it does not have to be generated *correctly* by this script
alone: every entry becomes a `using` declaration, so a name that does not exist
or is ambiguous fails the module's own compile. Over-inclusion is caught by the
compiler; under-inclusion is caught by whatever imports the module.

    python3 scripts/gen_module_exports.py            # rewrite modules/huxerui.cppm
    python3 scripts/gen_module_exports.py --check    # fail if it is out of date
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADERS = ROOT / "include" / "huxerui"
OUTPUT = ROOT / "modules" / "huxerui.cppm"
# The scope macros, lifted out of view.h so a module consumer can be handed them
# with -include. Generated for the same reason the module shell is: two copies
# of a macro definition that must match token for token is a drift waiting to
# happen, and view.h is the one that is public API.
PRELUDE = ROOT / "mcpp" / "huxerui-build-rules" / "include" / "huxerui_scope_prelude.h"
VIEW_HEADER = HEADERS / "view.h"
SCOPE_MACROS = ("HUXERUI_SCOPE", "HUXERUI_SCOPE_BEGIN", "HUXERUI_SCOPE_END")

# Headers that are platform-specific live in subdirectories and are explicit
# includes for their platform; the umbrella header does not pull them in, so
# neither does the module.
UMBRELLA = HEADERS / "huxerui.h"

BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT = re.compile(r"//[^\n]*")
RAW_STRING = re.compile(r'R"([^(]*)\((.*?)\)\1"', re.S)
# Braces inside literals are not scope. A single '}' in a char literal is enough
# to close a namespace that never opened and drop every later declaration.
STRING_LIT = re.compile(r'"(?:[^"\\\n]|\\.)*"')
CHAR_LIT = re.compile(r"'(?:[^'\\\n]|\\.)*'")
# A multi-line #define is skipped by its leading '#', but its CONTINUATION lines
# are not -- and HUXERUI_SCOPE_BEGIN's body carries an unbalanced '{'.
CONTINUED_DEFINE = re.compile(r"^[ \t]*#(?:[^\n\\]|\\\r?\n)*", re.M)

DECL_PATTERNS = [
    # class / struct, optionally after a template<> header on an earlier line
    re.compile(r"^(?:class|struct)\s+(?:\[\[[^\]]*\]\]\s*)?([A-Za-z_]\w*)\s*(?:final\b|:|\{|;)"),
    re.compile(r"^enum\s+(?:class\s+|struct\s+)?([A-Za-z_]\w*)\s*(?::|\{)"),
    re.compile(r"^using\s+([A-Za-z_]\w*)\s*="),
    re.compile(r"^(?:inline\s+|constexpr\s+|const\s+|extern\s+)*[A-Za-z_][\w:<>,\s*&\[\]]*?\b([A-Za-z_]\w*)\s*\("),
    re.compile(r"^(?:inline\s+)?constexpr\s+[A-Za-z_][\w:<>,\s*&]*\s+([A-Za-z_]\w*)\s*(?:=|\{)"),
]

KEYWORDS = {
    "if", "for", "while", "switch", "return", "sizeof", "static_cast", "else",
    "const_cast", "reinterpret_cast", "dynamic_cast", "noexcept", "operator",
    "explicit", "friend", "typedef", "namespace", "template", "public",
    "private", "protected", "virtual", "static", "inline", "constexpr",
    "consteval", "constinit", "decltype", "requires", "concept", "co_await",
    "co_return", "co_yield", "alignas", "alignof", "throw", "catch", "try",
    "new", "delete", "this", "true", "false", "nullptr", "auto", "void",
    "struct", "class", "enum", "union", "using", "extern", "export", "import",
    "module", "asm", "goto", "do", "case", "default", "break", "continue",
}


def strip_template_header(line: str) -> str:
    """Return what follows `template <...>`, or "" when the line is only that."""
    if not line.startswith("template"):
        return line
    i = line.find("<")
    if i < 0:
        return ""
    depth = 0
    for j in range(i, len(line)):
        if line[j] == "<":
            depth += 1
        elif line[j] == ">":
            depth -= 1
            if depth == 0:
                return line[j + 1:].strip()
    return ""


def strip_comments_and_strings(text: str) -> str:
    text = BLOCK_COMMENT.sub("", text)
    text = RAW_STRING.sub('""', text)
    # Whole preprocessor directives, continuations included, before anything
    # else counts braces.
    text = CONTINUED_DEFINE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    text = LINE_COMMENT.sub("", text)
    text = STRING_LIT.sub('""', text)
    text = CHAR_LIT.sub("'x'", text)
    return text


def scan(path: Path) -> set[str]:
    """Namespace-scope names declared directly in `namespace huxerui`."""
    text = strip_comments_and_strings(path.read_text(errors="replace"))
    names: set[str] = set()

    depth = 0
    ns_stack: list[tuple[str, int]] = []   # (name, depth at which it opened)
    pending_template = False

    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue

        opening = bool(ns_stack) and ns_stack[-1][0] == "huxerui" and len(ns_stack) == 1
        # ns_stack records the depth INSIDE the namespace, so a declaration at
        # namespace scope sits at exactly that depth -- not one deeper. Getting
        # this wrong silently finds only the handful of names that happen to be
        # nested one level further.
        at_scope = opening and depth == ns_stack[-1][1]

        ns = re.match(r"namespace\s+([A-Za-z_][\w:]*)?\s*\{", line)
        if ns:
            ns_stack.append((ns.group(1) or "", depth + 1))
            depth += line.count("{") - line.count("}")
            continue

        if at_scope:
            if line.startswith("template"):
                # `template <class T> class State final : ...` is one line, so
                # strip the template header (balanced angle brackets) and match
                # what follows. A bare `template <...>` line leaves nothing, and
                # the declaration arrives on the next one.
                rest = strip_template_header(line)
                if rest:
                    line = rest
                else:
                    pending_template = True
                    depth += line.count("{") - line.count("}")
                    continue
            if at_scope or pending_template:
                for pattern in DECL_PATTERNS:
                    m = pattern.match(line)
                    if m:
                        name = m.group(1)
                        # An out-of-class member definition sits at namespace
                        # scope but is not a namespace-scope NAME:
                        #   template <...> StringVariant StringVariant::Format(...)
                        # would otherwise export `Format`, which does not exist
                        # in namespace huxerui.
                        qualified = line[:m.start(1)].rstrip().endswith("::")
                        if not qualified and name not in KEYWORDS \
                           and not name.startswith("_"):
                            names.add(name)
                        break
            pending_template = False

        before = depth
        depth += line.count("{") - line.count("}")
        if depth < before:
            while ns_stack and depth < ns_stack[-1][1]:
                ns_stack.pop()

    return names


def umbrella_headers() -> list[Path]:
    text = strip_comments_and_strings(UMBRELLA.read_text())
    out = []
    for m in re.finditer(r"#\s*include\s*<huxerui/([^>]+)>", UMBRELLA.read_text()):
        p = HEADERS / m.group(1)
        if p.is_file():
            out.append(p)
    return out


def collect() -> list[str]:
    names: set[str] = set()
    for header in umbrella_headers():
        names |= scan(header)
    return sorted(names)


def render(names: list[str]) -> str:
    body = "\n".join(f"    using huxerui::{n};" for n in names)
    return f"""// GENERATED by scripts/gen_module_exports.py -- do not edit by hand.
//
// The C++20 module front door for HuxerUI. `import huxerui;` and
// `#include <huxerui/huxerui.h>` describe the SAME entities: the headers are
// included in the global module fragment below, so everything they declare
// stays attached to the global module and keeps its ordinary linkage and
// mangling. The module therefore changes no ABI and needs no separate library.
//
// Regenerate with:
//
//     python3 scripts/gen_module_exports.py
//
// mcpp/parity/check_parity.py fails when this file is out of date.

module;

#include <huxerui/huxerui.h>

export module huxerui;

export namespace huxerui {{
{body}
}}  // namespace huxerui
"""


def extract_scope_macros() -> str:
    """The three scope macros, verbatim, in the order view.h declares them."""
    text = VIEW_HEADER.read_text()
    out = []
    for name in SCOPE_MACROS:
        # A definition runs to the first line that does not end in a backslash.
        m = re.search(rf"^#define {name}\b.*$", text, re.M)
        if not m:
            raise SystemExit(f"{name} is no longer defined in {VIEW_HEADER.name}")
        lines = [m.group(0)]
        pos = m.end()
        while lines[-1].rstrip().endswith("\\"):
            nl = text.index("\n", pos) + 1
            end = text.find("\n", nl)
            end = len(text) if end < 0 else end
            lines.append(text[nl:end])
            pos = end
        out.append("\n".join(lines))
    return "\n\n".join(out)


def render_prelude(macros: str) -> str:
    return f"""// GENERATED by scripts/gen_module_exports.py -- do not edit by hand.
//
// The scope macros, extracted verbatim from <huxerui/view.h>.
//
// A macro does not cross a module boundary, so a translation unit that writes
// `import huxerui;` sees none of these -- and they are public API: hand-written
// composables use HUXERUI_SCOPE directly. huxerui.rules force-includes this
// file for every consumer, which is what makes `import huxerui;` and
// `#include <huxerui/huxerui.h>` behave the same.
//
// It is GENERATED rather than written because the definitions must match
// view.h's token for token: a translation unit that both imports the module and
// includes view.h would otherwise hit an illegal macro redefinition. Nothing
// here is a second source of truth.

#pragma once

{macros}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true",
                        help="fail if modules/huxerui.cppm is out of date")
    args = parser.parse_args()

    names = collect()
    if not names:
        print("no public names found -- the scanner is broken, refusing to write",
              file=sys.stderr)
        return 1
    content = render(names)
    prelude = render_prelude(extract_scope_macros())

    if args.check:
        stale = [p.relative_to(ROOT) for p, want in
                 ((OUTPUT, content), (PRELUDE, prelude))
                 if not p.is_file() or p.read_text() != want]
        if stale:
            print("out of date; run `python3 scripts/gen_module_exports.py`: "
                  + ", ".join(str(p) for p in stale), file=sys.stderr)
            return 1
        print(f"module shell is up to date ({len(names)} exported names, "
              f"{len(SCOPE_MACROS)} scope macros)")
        return 0

    for path, want in ((OUTPUT, content), (PRELUDE, prelude)):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(want)
    print(f"wrote {OUTPUT.relative_to(ROOT)} ({len(names)} exported names)")
    print(f"wrote {PRELUDE.relative_to(ROOT)} ({len(SCOPE_MACROS)} scope macros)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
