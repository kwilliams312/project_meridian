"""Regression tests for the Codex build-log warning classifier (#720)."""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCANNER = REPO / "tools" / "codex" / "check-warning-clean.sh"


def scan(tmp_path: Path, text: str) -> subprocess.CompletedProcess[str]:
    log = tmp_path / "build.log"
    log.write_text(text, encoding="utf-8")
    return subprocess.run(
        [str(SCANNER), "--scan-output", str(log)],
        text=True,
        capture_output=True,
        check=False,
    )


@pytest.mark.unit
@pytest.mark.parametrize(
    "diagnostic",
    [
        "tool.py:1: UserWarning: synthetic\n",
        "<frozen runpy>:128: RuntimeWarning: module was pre-imported\n",
        "CMake Warning at CMakeLists.txt:12 (message):\n  synthetic\n",
        "CMake Warning (dev) at CMakeLists.txt:12 (message):\n  synthetic\n",
        "CMake Deprecation Warning at CMakeLists.txt:1 (cmake_minimum_required):\n",
        "WARN Retry attempt #0. Sleeping 1.0s before the next attempt\n",
        "Model.cs(1,1): warning CS0618: obsolete\n",
        "project.csproj : warning NU1900: audit unavailable\n",
        "source.cpp:4:2: warning: unused variable\n",
    ],
)
def test_scanner_rejects_supported_toolchain_warning_forms(
    tmp_path: Path, diagnostic: str
) -> None:
    result = scan(tmp_path, diagnostic)
    assert result.returncode == 1
    assert "warning output detected" in result.stderr


@pytest.mark.unit
@pytest.mark.parametrize(
    "output",
    [
        "Build succeeded.\n    0 Warning(s)\n    0 Error(s)\n",
        "/tmp/story-720-codex-warning-clean/ContentTypes.g.cs\n",
        "Codex warning-clean assertion passed.\n",
        "CMake: configuring warning policy target\n",
    ],
)
def test_scanner_accepts_benign_warning_words(tmp_path: Path, output: str) -> None:
    result = scan(tmp_path, output)
    assert result.returncode == 0, result.stderr
    assert "assertion passed" in result.stdout
