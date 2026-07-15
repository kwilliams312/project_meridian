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

NPC_SIDECAR = """\
schema: meridian/asset@1
id: tp:art.char.kobold.miner
class: character_model
source: assets/art/char/kobold/miner.glb
license: CC-BY-4.0
provenance:
  source_tier: original
  authors: [tester]
"""

NPC_DOC = """\
schema: meridian/npc@1
id: tp:npc.kobold_miner
name: Kobold Miner
level: { min: 1, max: 2 }
creature_type: humanoid
rank: normal
faction: hostile
visual:
  model: tp:art.char.kobold.miner
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
        (pack / "assets" / "art" / "item" / "armor" / "plate.glb").write_bytes(
            b"glTF-bytes"
        )
    # The staged mount dir is named after the pack namespace (real layout:
    # client/project/meridian/<namespace> — ContentDB mounts res://meridian/<ns>),
    # so the coverage gate scopes to this mount's own pack.
    staged_dir = tmp_path / "tp"
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


def _npc_tree(tmp_path: Path, *, with_source: bool, staged: bool) -> tuple[Path, Path]:
    """A pack whose ONLY renderable ref is an NPC visual.model (no items/appearance)."""
    content = tmp_path / "content"
    pack = content / "tp"
    (pack / "assets" / "art" / "char" / "kobold").mkdir(parents=True)
    (pack / "npcs").mkdir(parents=True)
    (pack / "pack.yaml").write_text(PACK)
    (pack / "assets" / "art" / "kobold.asset.yaml").write_text(NPC_SIDECAR)
    (pack / "npcs" / "kobold_miner.npc.yaml").write_text(NPC_DOC)
    if with_source:
        (pack / "assets" / "art" / "char" / "kobold" / "miner.glb").write_bytes(
            b"glTF-bytes"
        )
    staged_dir = tmp_path / "tp"  # mount dir == pack namespace (real layout)
    if staged:
        dst = staged_dir / "art" / "char" / "kobold" / "miner.glb"
        dst.parent.mkdir(parents=True)
        dst.write_bytes(b"glTF-bytes")
    return content, staged_dir


@pytest.mark.unit
def test_npc_committed_but_unstaged_model_fails(tmp_path):
    # An NPC's visual.model is collected the same as an item's: committed source
    # bytes with no staged copy must fail the gate (issue #583 — the gap this closes).
    content, staged = _npc_tree(tmp_path, with_source=True, staged=False)
    failures = check(content, staged)
    assert len(failures) == 1
    assert "tp:art.char.kobold.miner" in failures[0]
    assert "NOT staged" in failures[0]


@pytest.mark.unit
def test_npc_placeholder_without_committed_bytes_is_skipped(tmp_path):
    # The M0 reality: every NPC/creature model is a byteless placeholder → skipped,
    # so extending the collector to NPCs is inert today (no behaviour change).
    content, staged = _npc_tree(tmp_path, with_source=False, staged=False)
    assert check(content, staged) == []


@pytest.mark.unit
def test_cross_pack_model_is_scoped_out(tmp_path):
    # A model referenced by content is owned by ITS pack's mount, not another's.
    # Here the `tp` pack's item references a model whose sidecar lives in a DIFFERENT
    # pack (`other`) with committed bytes; checking the `tp` mount must NOT flag it —
    # the `other` mount owns it (this is the chibi-body-under-core-mount case #759).
    content = tmp_path / "content"
    # The tp pack: item references other:art.item.armor.plate.
    tp = content / "tp"
    (tp / "items").mkdir(parents=True)
    (tp / "pack.yaml").write_text(PACK)
    (tp / "items" / "plate.item.yaml").write_text(
        ARMOR_ITEM.replace("tp:art.item.armor.plate", "other:art.item.armor.plate")
    )
    # The other pack: owns the sidecar + committed bytes, but is NOT the staged mount.
    other = content / "other"
    (other / "assets" / "art" / "item" / "armor").mkdir(parents=True)
    (other / "pack.yaml").write_text(PACK.replace("namespace: tp", "namespace: other"))
    (other / "assets" / "art" / "plate.asset.yaml").write_text(
        ARMOR_SIDECAR.replace("tp:art.item.armor.plate", "other:art.item.armor.plate")
    )
    (other / "assets" / "art" / "item" / "armor" / "plate.glb").write_bytes(
        b"glTF-bytes"
    )
    # Staged mount is `tp` (dir name == namespace); other:* belongs to the other mount.
    assert check(content, tmp_path / "tp") == []


@pytest.mark.unit
def test_real_tree_warden_kit_is_covered():
    # The committed repo tree must pass — every Warden plate is staged.
    failures = check(
        REPO / "content", REPO / "client" / "project" / "meridian" / "core"
    )
    assert failures == [], failures
