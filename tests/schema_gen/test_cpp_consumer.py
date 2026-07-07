"""Requirement (b), C++ target: compile the generated header with a real consumer.

Compiles fixtures/npc_consumer.cpp (which populates a generated `Npc` struct from
the real quartermaster_bren content entity) against the checked-in generated
header under C++20, then runs it. A green run proves the generated typed model
both compiles for mcc/worldd and faithfully represents real /content data.

Skipped (not failed) when no C++ compiler is available, so the pure-Python suite
still runs everywhere; CI on macOS/Linux has clang++/g++ and exercises it.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent.parent
GEN_CPP_DIR = REPO / "tools" / "schema_gen" / "generated" / "cpp"
CONSUMER = Path(__file__).resolve().parent / "fixtures" / "npc_consumer.cpp"


def _compiler() -> str | None:
    for cc in ("c++", "clang++", "g++"):
        if shutil.which(cc):
            return cc
    return None


@pytest.mark.integration
def test_generated_cpp_header_compiles_and_runs(tmp_path: Path):
    cc = _compiler()
    if cc is None:
        pytest.skip("no C++ compiler on PATH")

    exe = tmp_path / "npc_consumer"
    compile_cmd = [
        cc,
        "-std=c++20",
        "-Wall",
        "-Wextra",
        "-Werror",
        f"-I{GEN_CPP_DIR}",
        str(CONSUMER),
        "-o",
        str(exe),
    ]
    proc = subprocess.run(compile_cmd, capture_output=True, text=True)
    assert proc.returncode == 0, (
        f"generated C++ header failed to compile:\n{proc.stderr}"
    )

    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, f"consumer asserts failed:\n{run.stderr}"
