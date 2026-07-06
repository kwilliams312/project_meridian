#!/usr/bin/env python3
"""Doc-sync checker: baseline feature matrix vs. per-track PRD traceability tables.

Enforces two rules from the Baseline:

  1. Every feature the matrix marks ● for a track appears in that track's PRD
     traceability table (Baseline §4: "PRDs reference them in a traceability table").
  2. Every PRD's header references the CURRENT baseline version — a PRD revised
     against an older baseline must be re-reviewed and its reference bumped
     (the D-04 drift class: baseline absorbs a track's feedback, track never
     absorbs the baseline back).

Exit 1 on any violation. Runs in content CI next to validate_content.py.

Usage: python3 tools/check_traceability.py [--root <repo-root>]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

FEATURE_RE = re.compile(r"\b[A-Z]{2,4}-\d{2}\b")
TRACKS = ["Client", "Server", "Tools", "Art", "Music"]
PRD_FILES = {
    "Client": "docs/prd/client-prd.md",
    "Server": "docs/prd/server-prd.md",
    "Tools": "docs/prd/tools-prd.md",
    "Art": "docs/prd/art-prd.md",
    "Music": "docs/prd/music-prd.md",
}


def baseline_version(baseline: str) -> str:
    m = re.search(r"\*\*Version:\*\*\s*(\d+\.\d+)", baseline)
    if not m:
        raise SystemExit("cannot find baseline version header")
    return m.group(1)


def parse_matrix(baseline: str) -> dict[str, dict[str, bool]]:
    """Return {feature_id: {track: has_deliverable}} from the Baseline §4 table."""
    features: dict[str, dict[str, bool]] = {}
    for line in baseline.splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        # ID | Feature | Client | Server | Tools | Art | Music | Milestone
        if len(cells) != 8 or not FEATURE_RE.fullmatch(cells[0]):
            continue
        features[cells[0]] = {
            track: "●" in cells[i + 2] for i, track in enumerate(TRACKS)
        }
    return features


def traceability_ids(prd_text: str, prd_name: str) -> set[str]:
    """Feature IDs listed in the PRD's traceability section."""
    m = re.search(
        r"^#+\s*(?:\d+\.\s*)?Traceability.*?$", prd_text, re.MULTILINE | re.IGNORECASE
    )
    if not m:
        raise SystemExit(f"{prd_name}: no Traceability section found")
    section = prd_text[m.end() :]
    nxt = re.search(r"^## ", section, re.MULTILINE)
    if nxt:
        section = section[: nxt.start()]
    return set(FEATURE_RE.findall(section))


def prd_baseline_ref(prd_text: str) -> str | None:
    """Version cited on the PRD's `**Baseline:**` header line (not changelog mentions)."""
    for line in prd_text.splitlines()[:12]:
        if line.startswith("**Baseline:**"):
            m = re.search(r"v(\d+\.\d+)", line)
            return m.group(1) if m else None
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parent.parent
    )
    args = parser.parse_args()

    baseline = (args.root / "docs/00-GAME-DESIGN-BASELINE.md").read_text(
        encoding="utf-8"
    )
    version = baseline_version(baseline)
    matrix = parse_matrix(baseline)
    if not matrix:
        raise SystemExit("failed to parse the baseline feature matrix")

    problems: list[str] = []
    for track, prd_path in PRD_FILES.items():
        text = (args.root / prd_path).read_text(encoding="utf-8")

        ref = prd_baseline_ref(text)
        if ref != version:
            problems.append(
                f"{prd_path}: header references baseline v{ref or '?'} but current is v{version} "
                f"— re-review the PRD against the new baseline and bump the reference"
            )

        listed = traceability_ids(text, prd_path)
        owed = {fid for fid, marks in matrix.items() if marks[track]}
        for fid in sorted(owed - listed):
            problems.append(
                f"{prd_path}: baseline marks {fid} ● for {track} but the traceability table has no row for it"
            )

    print(
        f"Baseline v{version}: {len(matrix)} features checked against {len(PRD_FILES)} PRDs."
    )
    if problems:
        print(f"\n{len(problems)} problem(s):")
        for p in problems:
            print(f"  {p}")
        return 1
    print(
        "OK — every ● feature is claimed and every PRD is synchronized with the current baseline."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
