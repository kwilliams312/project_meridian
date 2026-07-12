"""Tests for tools/check_staged_models.py — staged-model coverage gate (#569).

The gate fails when a model whose source .glb bytes ARE committed to content/ has
no staged copy in the client pack (the invisible-Warden's-Kit regression), but must
NOT fail on placeholders (referenced models whose source is not yet committed).
"""

import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

from check_staged_models import check  # noqa: E402

PACK = """\
schema: meridian/pack@1
namespace: tp
name: Test Pack
version: 0.1.0
content_schema_version: 1
engine:
  godot: "4.6"
license: Apache-2.0
"""

ARMOR_SIDECAR = """\
schema: meridian/asset@1
id: tp:art.item.armor.plate
class: armor_model
source: assets/art/item/armor/plate.glb
license: CC-BY-4.0
provenance:
  source_tier: original
  authors: [tester]
"""

ARMOR_ITEM = """\
schema: meridian/item@2
id: tp:item.plate
name: Plate
item_class: armor
slot: chest
rarity: common
visual:
  icon: tp:art.icon.plate
  worn:
    models: [{ model: tp:art.item.armor.plate, mirror: none }]
    hides: [torso]
"""


def _tree(tmp_path: Path, *, with_source: bool, staged: bool) -> tuple[Path, Path]:
    content = tmp_path / "content"
    pack = content / "tp"
    (pack / "assets" / "art" / "item" / "armor").mkdir(parents=True)
    (pack / "items").mkdir(parents=True)
    (pack / "pack.yaml").write_text(PACK)
    (pack / "assets" / "art" / "plate.asset.yaml").write_text(ARMOR_SIDECAR)
    (pack / "items" / "plate.item.yaml").write_text(ARMOR_ITEM)
    if with_source:
        (pack / "assets" / "art" / "item" / "armor" / "plate.glb").write_bytes(b"glTF-bytes")
    staged_dir = tmp_path / "staged"
    if staged:
        dst = staged_dir / "art" / "item" / "armor" / "plate.glb"
        dst.parent.mkdir(parents=True)
        dst.write_bytes(b"glTF-bytes")
    return content, staged_dir


@pytest.mark.unit
def test_committed_but_unstaged_model_fails(tmp_path):
    content, staged = _tree(tmp_path, with_source=True, staged=False)
    failures = check(content, staged)
    assert len(failures) == 1
    assert "tp:art.item.armor.plate" in failures[0]
    assert "NOT staged" in failures[0]


@pytest.mark.unit
def test_committed_and_staged_model_passes(tmp_path):
    content, staged = _tree(tmp_path, with_source=True, staged=True)
    assert check(content, staged) == []


@pytest.mark.unit
def test_placeholder_without_committed_bytes_is_skipped(tmp_path):
    # Referenced model, but its source .glb is NOT committed → known M0 placeholder,
    # unavailable everywhere; not a staging regression.
    content, staged = _tree(tmp_path, with_source=False, staged=False)
    assert check(content, staged) == []


@pytest.mark.unit
def test_real_tree_warden_kit_is_covered():
    # The committed repo tree must pass — every Warden plate is staged.
    failures = check(REPO / "content", REPO / "client" / "project" / "meridian" / "core")
    assert failures == [], failures
