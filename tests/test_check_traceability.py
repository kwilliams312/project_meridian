"""Tests for tools/check_traceability.py — baseline matrix parsing and PRD sync rules."""

import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

from check_traceability import (  # noqa: E402
    baseline_version,
    parse_matrix,
    prd_baseline_ref,
    traceability_ids,
)

BASELINE = (REPO / "docs/00-GAME-DESIGN-BASELINE.md").read_text(encoding="utf-8")


@pytest.mark.unit
class TestMatrixParsing:
    def test_parses_all_features(self):
        # Floor, not a literal — the matrix grows with baseline revisions (53 at v0.4).
        matrix = parse_matrix(BASELINE)
        assert len(matrix) >= 53
        assert "ACC-01" in matrix and "OPS-04" in matrix

    def test_deliverable_vs_consulted(self):
        matrix = parse_matrix(BASELINE)
        # ACC-01: Client ●, Server ●, Tools ○ (consulted, not owed)
        assert matrix["ACC-01"]["Client"] is True
        assert matrix["ACC-01"]["Server"] is True
        assert matrix["ACC-01"]["Tools"] is False
        # WLD-04 (D-23): Client + Server ●, Tools ○
        assert matrix["WLD-04"]["Client"] is True
        assert matrix["WLD-04"]["Tools"] is False

    def test_baseline_version_parsed(self):
        # Format check, not a literal — the version bumps with every baseline revision.
        import re

        assert re.fullmatch(r"\d+\.\d+", baseline_version(BASELINE))


@pytest.mark.unit
class TestPrdRules:
    def test_baseline_ref_reads_header_line_not_changelog(self):
        text = (
            "# PRD\n"
            "**Version:** 0.3 (folds in Baseline v0.2 additions)\n"
            "**Baseline:** [Game Design Baseline v0.4](x.md) binding.\n"
        )
        assert prd_baseline_ref(text) == "0.4"

    def test_missing_traceability_section_is_fatal(self):
        with pytest.raises(SystemExit):
            traceability_ids("# PRD\nno tables here\n", "x.md")

    def test_traceability_ids_scoped_to_section(self):
        text = (
            "## 1. Intro\nmentions CMB-99 outside the table\n"
            "## 2. Traceability Table\n| ACC-01 | thing | M0 |\n"
            "## 3. Risks\n| ZZZ-11 | not traceability |\n"
        )
        ids = traceability_ids(text, "x.md")
        assert "ACC-01" in ids
        assert "ZZZ-11" not in ids


@pytest.mark.integration
class TestRepoDocs:
    def test_all_prds_synchronized(self):
        """Every matrix ● is claimed by its track's PRD and all PRDs cite baseline v0.4."""
        matrix = parse_matrix(BASELINE)
        version = baseline_version(BASELINE)
        from check_traceability import PRD_FILES

        for track, prd_path in PRD_FILES.items():
            text = (REPO / prd_path).read_text(encoding="utf-8")
            assert prd_baseline_ref(text) == version, prd_path
            listed = traceability_ids(text, prd_path)
            owed = {fid for fid, marks in matrix.items() if marks[track]}
            assert owed - listed == set(), (
                f"{prd_path} missing: {sorted(owed - listed)}"
            )
