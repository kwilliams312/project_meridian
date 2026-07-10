"""Tests for meridian_rig.bones — the canonical bone table (spec ④ §2)."""
import sys
from pathlib import Path
import yaml

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools" / "blender" / "meridian_rig"))
import bones  # noqa: E402


def test_bone_count_is_63():
    assert len(bones.ALL_BONES) == 63
    assert len(bones.PROFILE_BONES) == 56
    assert len(bones.SOCKET_BONES) == 7

def test_hierarchy_is_wellformed_single_root():
    h = bones.hierarchy()
    roots = [n for n, p in h.items() if p is None]
    assert roots == ["Root"]
    names = set(h)
    assert all(p in names for p in h.values() if p is not None)

def test_socket_parents_match_spec():
    parents = {b.name: b.parent for b in bones.SOCKET_BONES}
    assert parents == {
        "socket_main_hand": "RightHand", "socket_off_hand": "LeftHand",
        "socket_shield": "LeftHand", "socket_back": "Chest",
        "socket_ranged": "Chest", "socket_hip_l": "Hips", "socket_hip_r": "Hips"}

def test_skeleton_defs_bones_matches_table():
    """Drift guard: schema/content/skeleton.defs.yaml bones == bones.py names."""
    defs = yaml.safe_load((REPO / "schema/content/skeleton.defs.yaml").read_text())
    assert defs["$defs"]["boneName"]["enum"] == bones.bone_names()

def test_unknown_profile_raises():
    import pytest
    with pytest.raises(ValueError, match="ardent_male"):
        bones.for_profile("dwarf_female")
