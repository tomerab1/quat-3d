#!/usr/bin/env python3
"""Check that THIRD_PARTY_NOTICES.md lists every third-party component.

Every vcpkg.json dependency, every CMake FetchContent_Declare package and
every font directory under assets/fonts/ needs a table row whose first cell
is its ID in backticks, with a non-empty licence cell. Rows that name nothing
the build uses any more are reported too, for the vcpkg and FetchContent
sections. Run by CI; exits 1 with one line per problem.

    scripts/check-third-party-notices.py [<repo-root>]
"""

import json
import re
import sys
from pathlib import Path

ROW = re.compile(r"^\|\s*`([^`]+)`\s*\|([^|]*)\|([^|]*)\|")
SECTION = re.compile(r"^##\s+(.*)$")
DECLARE = re.compile(r"FetchContent_Declare\(\s*([A-Za-z0-9_.+-]+)", re.IGNORECASE)


def notice_rows(text):
    """{id: (section, licence)} from the notices tables."""
    rows, section = {}, ""
    for line in text.splitlines():
        m = SECTION.match(line)
        if m:
            section = m.group(1)
            continue
        m = ROW.match(line)
        if m:
            rows[m.group(1).strip()] = (section, m.group(3).strip())
    return rows


def vcpkg_dependencies(root):
    manifest = json.loads((root / "vcpkg.json").read_text(encoding="utf-8"))
    names = set()
    for dep in manifest.get("dependencies", []):
        names.add(dep if isinstance(dep, str) else dep["name"])
    return names


def fetchcontent_packages(root):
    names = set()
    files = [root / "CMakeLists.txt", *sorted((root / "cmake").rglob("*.cmake"))]
    files += sorted(root.glob("src/**/CMakeLists.txt"))
    for f in files:
        if f.is_file():
            names.update(n.lower() for n in DECLARE.findall(f.read_text(encoding="utf-8")))
    return names


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent)
    rows = notice_rows((root / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8"))
    problems = []

    vcpkg = vcpkg_dependencies(root)
    fetched = fetchcontent_packages(root)
    fonts = {d.name for d in (root / "assets" / "fonts").iterdir() if d.is_dir()}
    required = [("vcpkg dependency", vcpkg), ("FetchContent package", fetched),
                ("font directory assets/fonts/", fonts)]
    for kind, names in required:
        for name in sorted(names):
            if name not in rows:
                problems.append(f"{kind} '{name}' has no row in THIRD_PARTY_NOTICES.md")
            elif not rows[name][1]:
                problems.append(f"'{name}' has no licence in THIRD_PARTY_NOTICES.md")

    for name, (section, _) in sorted(rows.items()):
        if section.startswith("Libraries (vcpkg") and name not in vcpkg:
            problems.append(f"THIRD_PARTY_NOTICES.md lists '{name}' as a vcpkg dependency, but vcpkg.json does not")
        if section.startswith("Libraries fetched") and name not in fetched:
            problems.append(f"THIRD_PARTY_NOTICES.md lists '{name}' as a FetchContent package, but no FetchContent_Declare names it")

    for p in problems:
        print(f"::error file=THIRD_PARTY_NOTICES.md::{p}")
    if problems:
        return 1
    print(f"THIRD_PARTY_NOTICES.md: {len(vcpkg)} vcpkg, {len(fetched)} FetchContent, {len(fonts)} font entries present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
