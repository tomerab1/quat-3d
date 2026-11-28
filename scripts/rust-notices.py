#!/usr/bin/env python3
"""Licences of quat-mcp's Rust dependencies, from `cargo metadata`.

    scripts/rust-notices.py check    <Cargo.toml>
    scripts/rust-notices.py markdown <Cargo.toml> <out.md>

check: every package in Cargo.lock (all platforms, all dependency kinds)
must declare a licence (`license`, or `license-file`), and every declared
SPDX expression must be satisfiable from ACCEPTED (an OR needs one accepted
side, an AND needs both). Run by CI's Rust job; exits 1 with one line per
problem.

markdown: the notices section for a release archive. It covers the crates
compiled into the quat-mcp binary for this host (normal dependencies, minus
proc-macros, which only run at build time), with a table of names, versions
and licences. Then come each crate's own licence files (LICENSE*, COPYING*,
NOTICE*, ...) from its source in the Cargo registry, after
`cargo build --locked` has downloaded it. A crate that ships no licence file
is covered by the reference text of its licence, packaging/licenses/spdx/<ID>.txt.
Every crate ships its own today, so that folder does not exist yet; markdown
fails and names the file to add when a crate needs it. Identical texts are
written once. Used by scripts/third-party-notices.sh.
"""

import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPDX_TEXTS = ROOT / "packaging" / "licenses" / "spdx"

# Licences quat-mcp may ship. Any other licence fails the check, so adding one
# is a deliberate decision recorded here. All are permissive except MPL-2.0,
# which is file-level copyleft: the binary may ship under MIT as long as the
# MPL-covered source files stay available (they are, unmodified, on
# crates.io) and their licence is in the notices. It comes in through
# option-ext (dirs -> dirs-sys).
ACCEPTED = {
    "0BSD", "Apache-2.0", "Apache-2.0 WITH LLVM-exception", "BSD-2-Clause",
    "BSD-3-Clause", "BSL-1.0", "CC0-1.0", "CDLA-Permissive-2.0", "ISC", "MIT",
    "MIT-0", "MPL-2.0", "Unicode-3.0", "Unicode-DFS-2016", "Unlicense", "Zlib",
}
LICENCE_FILE = re.compile(r"^(LICEN[CS]E|COPYING|NOTICE|UNLICENSE)", re.IGNORECASE)


def cargo_metadata(manifest, host_only):
    cmd = ["cargo", "metadata", "--format-version", "1", "--locked",
           "--manifest-path", str(manifest)]
    if host_only:
        host = subprocess.run(["rustc", "-vV"], capture_output=True, text=True, check=True,
                              cwd=manifest.parent).stdout
        triple = re.search(r"^host: (\S+)$", host, re.MULTILINE).group(1)
        cmd += ["--filter-platform", triple]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True, cwd=manifest.parent)
    return json.loads(out.stdout)


# --- SPDX expressions: OR / AND / WITH / parentheses; "/" is old-style OR. ---

def tokens(expr):
    return re.findall(r"\(|\)|[^\s()]+", expr.replace("/", " OR "))


def satisfiable(expr):
    """True when the expression can be met with ACCEPTED licences only."""
    toks = tokens(expr)
    pos = 0

    def peek():
        return toks[pos] if pos < len(toks) else None

    def take():
        nonlocal pos
        pos += 1
        return toks[pos - 1]

    def primary():
        if peek() == "(":
            take()
            value = disjunction()
            if take() != ")":
                raise ValueError("unbalanced parentheses")
            return value
        ident = take()
        if peek() == "WITH":
            take()
            ident = f"{ident} WITH {take()}"
        return ident in ACCEPTED or ident.split(" WITH ")[0] in ACCEPTED and "WITH" in ident

    def conjunction():
        value = primary()
        while peek() == "AND":
            take()
            value = primary() and value
        return value

    def disjunction():
        value = conjunction()
        while peek() == "OR":
            take()
            value = conjunction() or value
        return value

    try:
        result = disjunction()
    except (IndexError, ValueError):
        return False
    return result and pos == len(toks)


def check(manifest):
    meta = cargo_metadata(manifest, host_only=False)
    root = meta["resolve"]["root"]
    problems = []
    for pkg in sorted(meta["packages"], key=lambda p: (p["name"], p["version"])):
        if pkg["id"] == root:
            continue
        name = f"{pkg['name']} {pkg['version']}"
        if pkg.get("license"):
            if not satisfiable(pkg["license"]):
                problems.append(f"{name}: licence '{pkg['license']}' is not in the accepted set "
                                "(scripts/rust-notices.py ACCEPTED)")
        elif not pkg.get("license_file"):
            problems.append(f"{name}: declares no licence")
    for p in problems:
        print(f"::error file=tools/quat-mcp/Cargo.lock::{p}")
    if problems:
        return 1
    print(f"quat-mcp: {len(meta['packages']) - 1} Rust packages, all with accepted licences")
    return 0


def runtime_packages(meta):
    """Packages linked into the binary: normal deps reachable from the root, no proc-macros."""
    packages = {p["id"]: p for p in meta["packages"]}
    nodes = {n["id"]: n for n in meta["resolve"]["nodes"]}
    root = meta["resolve"]["root"]
    seen, stack = set(), [root]
    while stack:
        current = stack.pop()
        if current in seen:
            continue
        seen.add(current)
        for dep in nodes[current]["deps"]:
            if not any(kind["kind"] is None for kind in dep["dep_kinds"]):
                continue
            pkg = packages[dep["pkg"]]
            if any("proc-macro" in target["kind"] for target in pkg["targets"]):
                continue
            stack.append(dep["pkg"])
    seen.discard(root)
    return sorted((packages[i] for i in seen), key=lambda p: (p["name"], p["version"]))


def licence_files(pkg):
    src = Path(pkg["manifest_path"]).parent
    files = sorted(f for f in src.iterdir() if f.is_file() and LICENCE_FILE.match(f.name))
    if pkg.get("license_file"):
        declared = (src / pkg["license_file"]).resolve()
        if declared.is_file() and declared not in [f.resolve() for f in files]:
            files.append(declared)
    return files


def spdx_ids(expr):
    return sorted({t for t in tokens(expr) if t not in ("(", ")", "OR", "AND", "WITH")
                   and not t.endswith("-exception")})


def fenced(text):
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    if not text.endswith("\n"):
        text += "\n"
    return f"````text\n{text}````\n"


def markdown(manifest, out):
    meta = cargo_metadata(manifest, host_only=True)
    crates = runtime_packages(meta)
    lines = ["\n### quat-mcp (Rust crates)\n",
             "\nThe crates compiled into `quat-mcp` for this platform, from `Cargo.lock`.\n",
             "\n| Crate | Version | Licence |\n|---|---|---|\n"]
    for pkg in crates:
        lines.append(f"| {pkg['name']} | {pkg['version']} | {pkg.get('license') or 'see its licence file'} |\n")

    # A text many crates share verbatim (the Apache-2.0 licence, say) is
    # written once; later crates point back to the first one that carried it.
    reference, first_seen = set(), {}
    for pkg in crates:
        files = licence_files(pkg)
        heading = f"{pkg['name']} {pkg['version']}"
        lines.append(f"\n#### {heading}\n")
        if not files:
            ids = spdx_ids(pkg.get("license") or "")
            lines.append(f"\nThe crate ships no licence file; it is licensed under "
                         f"`{pkg.get('license')}`, whose reference text is below.\n")
            reference.update(ids)
            continue
        for f in files:
            text = f.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n").strip()
            if text in first_seen:
                lines.append(f"\n`{f.name}`: the same text as {first_seen[text]}.\n")
                continue
            first_seen[text] = f"`{f.name}` of {heading}"
            lines.append(f"\n`{f.name}`\n\n")
            lines.append(fenced(text))

    if reference:
        lines.append("\n#### Reference licence texts\n")
        for ident in sorted(reference):
            path = SPDX_TEXTS / f"{ident}.txt"
            if not path.is_file():
                print(f"no reference text {path} for licence {ident}", file=sys.stderr)
                return 1
            lines.append(f"\n`{ident}`\n\n")
            lines.append(fenced(path.read_text(encoding="utf-8")))

    Path(out).write_text("".join(lines), encoding="utf-8", newline="\n")
    return 0


def main(argv):
    if len(argv) == 3 and argv[1] == "check":
        return check(Path(argv[2]).resolve())
    if len(argv) == 4 and argv[1] == "markdown":
        return markdown(Path(argv[2]).resolve(), argv[3])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    os.environ.setdefault("CARGO_TERM_COLOR", "never")
    sys.exit(main(sys.argv))
