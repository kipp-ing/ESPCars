#!/usr/bin/env python3
"""Fail if any git-tracked file names a real vehicle, a real bench's diagnostic
data, or a sibling private repo by path (docs/CONVENTIONS.md's data-hygiene
rule).

Generic, textbook or fictional diagnostic examples are fine — a DID, a tester
address, a field name. What is never allowed in this repo's history is
something that ties an example to one real vehicle or ECU: a real vehicle
model name, a real factory database's tool/vendor name, or an absolute path
into a sibling repo that holds real reverse-engineered data. That real data
lives under the gitignored `private/` directory instead.

Run via `script/check.sh`, or directly:
    script/check_no_private_data.py
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SELF = Path(__file__).resolve()

# Each pattern is (regex, human reason). Case-insensitive. Keep this list of
# *identity* markers, not generic protocol values (DIDs, tester addresses) —
# those are legitimately reusable in fictional examples and would false-positive.
#
# A history rewrite scrubs old commits with a literal find/replace pass over
# every blob, this file included (it is source, not just a scanner). So no
# pattern or reason below may spell one of its own trigger phrases out as a
# contiguous run of characters: that same rewrite would blind-replace it right
# here and corrupt either the regex or the human-readable text. Where a regex
# needs to detect a contiguous word in *other* files, an empty non-capturing
# group `(?:)` is spliced into the middle of it — a true zero-width no-op that
# changes nothing about what the pattern matches, but breaks the literal byte
# run in this file's own source.
PATTERNS: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"smart[ _-]?451", re.I), "names the real bench vehicle by manufacturer and chassis code"),
    (re.compile(r"fort(?:)two", re.I), "names the real bench vehicle's marketing name"),
    (re.compile(r"\bsmart[ _-]?ed\b", re.I), "names the real bench vehicle's marketing name, chassis code omitted"),
    (re.compile(r"\bdaim(?:)ler\b", re.I), "names the real vehicle's manufacturer"),
    (re.compile(r"mcc[-_ ]?smart", re.I), "names the real vehicle's manufacturing entity"),
    (re.compile(r"\bov(?:)ms\b", re.I), "names a real reverse-engineering project tied to the bench vehicle"),
    (re.compile(r"ed_(?:)bmsdiag", re.I), "names a real reverse-engineering project tied to the bench vehicle"),
    (re.compile(r"cbf.convert", re.I), "names the private factory-database conversion tool by name"),
    (re.compile(r"/dev/repos/(cbf.convert|smart.diag)\b"), "hardcodes an absolute path into a sibling private repo"),
    (re.compile(r"(?<!private/)catalogs/bms(?:)-live\."), "references the real compiled bench catalog at a PUBLIC path"),
    (re.compile(r"\bPNHV(?:)_"), "names real CAN signal identifiers from the bench vehicle's factory DBC"),
    (re.compile(r"smart.ev.battery.rig", re.I), "names a real sibling simulator project tied to the bench vehicle"),
]

# Extensions worth scanning as text; skip binaries entirely.
TEXT_SUFFIXES = {".py", ".md", ".yaml", ".yml", ".h", ".hpp", ".cpp", ".cc", ".json", ".txt", ".sh", ".cfg", ".ini"}


def tracked_files() -> list[Path]:
    out = subprocess.run(
        ["git", "ls-files"], cwd=REPO_ROOT, capture_output=True, text=True, check=True
    )
    return [REPO_ROOT / line for line in out.stdout.splitlines() if line]


def main() -> int:
    violations: list[str] = []
    for path in tracked_files():
        if path == SELF:
            continue
        if path.suffix.lower() not in TEXT_SUFFIXES:
            continue
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        rel = path.relative_to(REPO_ROOT)
        for lineno, line in enumerate(text.splitlines(), start=1):
            for pattern, reason in PATTERNS:
                if pattern.search(line):
                    violations.append(f"{rel}:{lineno}: {reason}\n    {line.strip()}")

    if violations:
        print("Real/private diagnostic data found in tracked files (docs/CONVENTIONS.md):\n")
        print("\n".join(violations))
        print(
            "\nMove real data under the gitignored private/ directory, or use a "
            "fictional example instead (see the mini fixture in tests/uds/fixtures/)."
        )
        return 1

    print(f"OK: no private-data markers in {len(tracked_files())} tracked files.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
