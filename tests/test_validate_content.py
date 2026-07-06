"""Tests for tools/validate_content.py — one negative fixture per lint rule (Tools PRD §11.1)."""

import sys
import textwrap
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

from validate_content import validate  # noqa: E402

SCHEMA_DIR = REPO / "schema" / "content"

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

NPC = """\
schema: meridian/npc@1
id: tp:npc.dummy
name: Dummy
level: { min: 1, max: 1 }
creature_type: humanoid
faction: friendly
stats:
  health: 10
  damage: { min: 1, max: 2 }
  attack_speed_ms: 2000
"""

ZONE = """\
schema: meridian/zone@1
id: tp:zone.z1
name: Test Zone
level_range: { min: 1, max: 10 }
pois:
  - id: known_poi
    name: Known POI
    position: { x: 0, y: 0, z: 0 }
"""

QUEST = """\
schema: meridian/quest@1
id: tp:quest.q1
name: Quest One
summary: s
offer_text: o
completion_text: c
level: 1
giver: npc.dummy
objectives:
  - type: kill
    target: npc.dummy
    count: 1
rewards:
  xp: 10
"""

SPAWN = """\
schema: meridian/spawn@1
id: tp:spawn.s1
zone: zone.z1
spawns:
  - npc: npc.dummy
    position: { x: 0, y: 0, z: 0 }
    respawn_seconds: { min: 60, max: 60 }
"""


def build(tmp_path: Path, files: dict[str, str], with_base: bool = True) -> Path:
    """Write a content tree under tmp_path/content/t and return the content dir."""
    content = tmp_path / "content"
    base = {"tp/pack.yaml": PACK} if with_base else {}
    for relpath, text in {**base, **files}.items():
        p = content / relpath
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(textwrap.dedent(text), encoding="utf-8")
    return content


def run(tmp_path: Path, files: dict[str, str], assets_mode: str = "ignore", **kw):
    return validate(build(tmp_path, files, **kw), SCHEMA_DIR, assets_mode)


def codes(messages: list[str]) -> set[str]:
    return {m.split(" ", 1)[0] for m in messages}


@pytest.mark.unit
class TestBaseline:
    def test_valid_tree_passes(self, tmp_path):
        res = run(tmp_path, {"tp/npcs/dummy.npc.yaml": NPC})
        assert res.errors == []
        assert res.warnings == []

    def test_malformed_yaml_reports_parse_not_crash(self, tmp_path):
        res = run(tmp_path, {"tp/npcs/bad.npc.yaml": "a: [unclosed\n  b: {"})
        assert "PARSE" in codes(res.errors)

    def test_empty_file_reports_parse_not_crash(self, tmp_path):
        res = run(tmp_path, {"tp/npcs/empty.npc.yaml": ""})
        assert "PARSE" in codes(res.errors)

    def test_non_mapping_document_reports_parse(self, tmp_path):
        res = run(tmp_path, {"tp/npcs/list.npc.yaml": "- 1\n- 2\n"})
        assert "PARSE" in codes(res.errors)


@pytest.mark.unit
class TestFileAndEnvelope:
    def test_l001_bad_filename(self, tmp_path):
        res = run(tmp_path, {"tp/npcs/dummy.creature.yaml": NPC})
        assert "L001" in codes(res.errors)

    def test_l001_envelope_type_mismatch(self, tmp_path):
        res = run(
            tmp_path, {"tp/items/dummy.item.yaml": NPC}
        )  # npc envelope in .item.yaml
        assert "L001" in codes(res.errors)

    def test_schema_violation_reported(self, tmp_path):
        broken = NPC.replace("creature_type: humanoid\n", "")
        res = run(tmp_path, {"tp/npcs/dummy.npc.yaml": broken})
        assert "SCHEMA" in codes(res.errors)


@pytest.mark.unit
class TestIds:
    def test_l002_namespace_mismatch(self, tmp_path):
        res = run(
            tmp_path,
            {"tp/npcs/dummy.npc.yaml": NPC.replace("tp:npc.dummy", "other:npc.dummy")},
        )
        assert "L002" in codes(res.errors)

    def test_l002_no_pack_manifest(self, tmp_path):
        res = run(tmp_path, {"tp/npcs/dummy.npc.yaml": NPC}, with_base=False)
        assert "L002" in codes(res.errors)

    def test_l003_id_type_vs_schema_type(self, tmp_path):
        res = run(
            tmp_path,
            {"tp/npcs/dummy.npc.yaml": NPC.replace("tp:npc.dummy", "tp:item.dummy")},
        )
        assert "L003" in codes(res.errors)

    def test_l004_min_greater_than_max(self, tmp_path):
        res = run(
            tmp_path,
            {
                "tp/npcs/dummy.npc.yaml": NPC.replace(
                    "level: { min: 1, max: 1 }", "level: { min: 5, max: 1 }"
                )
            },
        )
        assert "L004" in codes(res.errors)

    def test_l010_duplicate_id(self, tmp_path):
        res = run(
            tmp_path,
            {
                "tp/npcs/dummy.npc.yaml": NPC,
                "tp/npcs/dummy2.npc.yaml": NPC,
            },
        )
        assert "L010" in codes(res.errors)


@pytest.mark.unit
class TestReferences:
    def test_l011_unresolved_content_ref(self, tmp_path):
        npc = NPC + "loot:\n  table: loot.nope\n"
        res = run(tmp_path, {"tp/npcs/dummy.npc.yaml": npc})
        assert "L011" in codes(res.errors)

    def test_l020_missing_sidecar_warns_by_default(self, tmp_path):
        npc = NPC + "visual:\n  model: art.char.missing\n"
        res = run(tmp_path, {"tp/npcs/dummy.npc.yaml": npc}, assets_mode="warn")
        assert "L020" in codes(res.warnings)
        assert res.errors == []

    def test_l020_missing_sidecar_errors_in_strict_mode(self, tmp_path):
        npc = NPC + "visual:\n  model: art.char.missing\n"
        res = run(tmp_path, {"tp/npcs/dummy.npc.yaml": npc}, assets_mode="error")
        assert "L020" in codes(res.errors)

    def test_l020_satisfied_by_sidecar(self, tmp_path):
        npc = NPC + "visual:\n  model: art.char.dummy\n"
        sidecar = """\
        schema: meridian/asset@1
        id: tp:art.char.dummy
        class: character_model
        source: assets/art/char/dummy.glb
        license: CC-BY-4.0
        provenance:
          source_tier: original
          authors: [tester]
        """
        res = run(
            tmp_path,
            {
                "tp/npcs/dummy.npc.yaml": npc,
                "tp/assets/art/char_dummy.asset.yaml": sidecar,
            },
            assets_mode="error",
        )
        assert res.errors == []

    def test_amb_prefix_recognized(self, tmp_path):
        zone = ZONE + "ambience: amb.z1.bed\n"
        res = run(tmp_path, {"tp/zones/z1.zone.yaml": zone}, assets_mode="warn")
        assert any("amb.z1.bed" in w for w in res.warnings)


@pytest.mark.unit
class TestSemantics:
    def test_l034_unknown_poi(self, tmp_path):
        quest = QUEST.replace(
            "  - type: kill\n    target: npc.dummy\n    count: 1",
            "  - type: explore\n    zone: zone.z1\n    poi: nope",
        )
        res = run(
            tmp_path,
            {
                "tp/npcs/dummy.npc.yaml": NPC,
                "tp/zones/z1.zone.yaml": ZONE,
                "tp/quests/q1.quest.yaml": quest,
            },
        )
        assert "L034" in codes(res.errors)

    def test_l034_known_poi_passes(self, tmp_path):
        quest = QUEST.replace(
            "  - type: kill\n    target: npc.dummy\n    count: 1",
            "  - type: explore\n    zone: zone.z1\n    poi: known_poi",
        )
        res = run(
            tmp_path,
            {
                "tp/npcs/dummy.npc.yaml": NPC,
                "tp/zones/z1.zone.yaml": ZONE,
                "tp/quests/q1.quest.yaml": quest,
            },
        )
        assert res.errors == []

    def test_l035_unspawned_giver_warns(self, tmp_path):
        giver = NPC.replace("tp:npc.dummy", "tp:npc.giver")
        spawn_other = SPAWN  # spawns npc.dummy only, not npc.giver
        quest = QUEST.replace("giver: npc.dummy", "giver: npc.giver")
        res = run(
            tmp_path,
            {
                "tp/npcs/dummy.npc.yaml": NPC,
                "tp/npcs/giver.npc.yaml": giver,
                "tp/zones/z1.zone.yaml": ZONE,
                "tp/spawns/s1.spawn.yaml": spawn_other,
                "tp/quests/q1.quest.yaml": quest,
            },
        )
        assert any("L035" in w and "npc.giver" in w for w in res.warnings)

    def test_l035_silent_without_any_spawn_files(self, tmp_path):
        res = run(
            tmp_path,
            {
                "tp/npcs/dummy.npc.yaml": NPC,
                "tp/quests/q1.quest.yaml": QUEST,
            },
        )
        assert "L035" not in codes(res.warnings)

    def test_l052_nested_table_referencing_tables(self, tmp_path):
        loot_a = "schema: meridian/loot@1\nid: tp:loot.a\nentries:\n  - table: loot.b\n    chance_pct: 50\n"
        loot_b = "schema: meridian/loot@1\nid: tp:loot.b\nentries:\n  - table: loot.c\n    chance_pct: 50\n"
        loot_c = "schema: meridian/loot@1\nid: tp:loot.c\nmoney: { min: 1, max: 2 }\n"
        res = run(
            tmp_path,
            {
                "tp/loot/a.loot.yaml": loot_a,
                "tp/loot/b.loot.yaml": loot_b,
                "tp/loot/c.loot.yaml": loot_c,
            },
        )
        assert "L052" in codes(res.errors)

    def test_l052_one_level_allowed(self, tmp_path):
        loot_a = "schema: meridian/loot@1\nid: tp:loot.a\nentries:\n  - table: loot.b\n    chance_pct: 50\n"
        loot_b = "schema: meridian/loot@1\nid: tp:loot.b\nmoney: { min: 1, max: 2 }\n"
        res = run(
            tmp_path,
            {
                "tp/loot/a.loot.yaml": loot_a,
                "tp/loot/b.loot.yaml": loot_b,
            },
        )
        assert res.errors == []

    def test_l062_vendor_item_without_price(self, tmp_path):
        item = """\
        schema: meridian/item@1
        id: tp:item.thing
        name: Thing
        item_class: trade_good
        rarity: common
        visual:
          icon: art.icon.thing
        """
        vendor = "schema: meridian/vendor@1\nid: tp:vendor.v1\nitems:\n  - item: item.thing\n"
        res = run(
            tmp_path,
            {
                "tp/items/thing.item.yaml": item,
                "tp/vendors/v1.vendor.yaml": vendor,
            },
        )
        assert "L062" in codes(res.errors)

    def test_l062_price_override_suffices(self, tmp_path):
        item = """\
        schema: meridian/item@1
        id: tp:item.thing
        name: Thing
        item_class: trade_good
        rarity: common
        visual:
          icon: art.icon.thing
        """
        vendor = (
            "schema: meridian/vendor@1\nid: tp:vendor.v1\nitems:\n"
            "  - item: item.thing\n    price_override: 100\n"
        )
        res = run(
            tmp_path,
            {
                "tp/items/thing.item.yaml": item,
                "tp/vendors/v1.vendor.yaml": vendor,
            },
        )
        assert res.errors == []


@pytest.mark.unit
class TestSchemaConstraints:
    def test_spawn_wander_and_patrol_mutually_exclusive(self, tmp_path):
        spawn = SPAWN.replace(
            "    respawn_seconds: { min: 60, max: 60 }",
            "    respawn_seconds: { min: 60, max: 60 }\n"
            "    wander_radius_m: 5\n"
            "    patrol:\n"
            "      waypoints:\n"
            "        - position: { x: 0, y: 0, z: 0 }\n"
            "        - position: { x: 1, y: 0, z: 1 }",
        )
        res = run(
            tmp_path,
            {
                "tp/npcs/dummy.npc.yaml": NPC,
                "tp/zones/z1.zone.yaml": ZONE,
                "tp/spawns/s1.spawn.yaml": spawn,
            },
        )
        assert "SCHEMA" in codes(res.errors)

    def test_asset_sidecar_music_requires_metadata(self, tmp_path):
        sidecar = """\
        schema: meridian/asset@1
        id: tp:mus.z1.explore
        class: music_stem
        source: assets/audio/mus/explore.wav
        license: CC-BY-4.0
        provenance:
          source_tier: original
          authors: [tester]
        """
        res = run(tmp_path, {"tp/assets/mus/z1_explore.asset.yaml": sidecar})
        assert "SCHEMA" in codes(
            res.errors
        )  # music + loudness blocks required for music_stem

    def test_asset_sidecar_ai_requires_ai_block(self, tmp_path):
        sidecar = """\
        schema: meridian/asset@1
        id: tp:art.prop.rock
        class: prop
        source: assets/art/prop/rock.glb
        license: CC-BY-4.0
        provenance:
          source_tier: ai
          authors: [tester]
          origin_url: https://example.com
        """
        res = run(tmp_path, {"tp/assets/art/prop_rock.asset.yaml": sidecar})
        assert "SCHEMA" in codes(
            res.errors
        )  # provenance.ai block required for source_tier: ai


@pytest.mark.integration
class TestRepoContent:
    def test_repo_content_validates_clean(self):
        res = validate(REPO / "content", SCHEMA_DIR, assets_mode="error")
        assert res.errors == [], res.errors
        assert res.warnings == [], res.warnings
