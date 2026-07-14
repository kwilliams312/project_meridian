"""Regression tests for the Codex build-log warning classifier (#720)."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCANNER = REPO / "tools" / "codex" / "check-warning-clean.sh"


def scan(
    tmp_path: Path, text: str, *, env: dict[str, str] | None = None
) -> subprocess.CompletedProcess[str]:
    log = tmp_path / "build.log"
    log.write_text(text, encoding="utf-8")
    return subprocess.run(
        [str(SCANNER), "--scan-output", str(log)],
        text=True,
        capture_output=True,
        check=False,
        env=env,
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
        "Model.cs(12,8): warning CA2000: Dispose objects before losing scope\n",
        "ViewModel.cs(21,9,21,24): warning IDE0058: Expression value is never used\n",
        "Tests.cs(7,5): warning xUnit1031: Do not use blocking task operations\n",
        "Rules.fs(4,2): warning FS0020: The result is implicitly ignored\n",
        "Legacy.vb(2,1): warning BC42024: Unused local variable\n",
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
        "Analyzer documentation: warning CA2000 explains disposal rules.\n",
        "/tmp/warning CA2000:/Model.cs\n",
    ],
)
def test_scanner_accepts_benign_warning_words(tmp_path: Path, output: str) -> None:
    result = scan(tmp_path, output)
    assert result.returncode == 0, result.stderr
    assert "assertion passed" in result.stdout


@pytest.mark.unit
@pytest.mark.parametrize("kind", ["missing", "directory"])
def test_scanner_rejects_non_file_input(tmp_path: Path, kind: str) -> None:
    candidate = tmp_path / "build.log"
    if kind == "directory":
        candidate.mkdir()

    result = subprocess.run(
        [str(SCANNER), "--scan-output", str(candidate)],
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 2
    assert "not a readable regular file" in result.stderr
    assert "assertion passed" not in result.stdout


@pytest.mark.unit
@pytest.mark.parametrize("scanner_status", [2, 7])
def test_scanner_propagates_scanner_failure_portably(
    tmp_path: Path, scanner_status: int
) -> None:
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    fake_grep = fake_bin / "grep"
    fake_grep.write_text(f"#!/bin/sh\nexit {scanner_status}\n", encoding="utf-8")
    fake_grep.chmod(0o755)
    env = os.environ.copy()
    env["PATH"] = f"{fake_bin}{os.pathsep}{env['PATH']}"

    result = scan(tmp_path, "Build succeeded.\n", env=env)

    assert result.returncode == 2
    assert f"warning scanner exited with status {scanner_status}" in result.stderr
    assert "assertion passed" not in result.stdout
