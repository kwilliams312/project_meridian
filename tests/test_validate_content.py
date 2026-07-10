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

# A fully-valid original-tier character sidecar; individual tests mutate one field.
ASSET = """\
schema: meridian/asset@1
id: tp:art.char.hero
class: character_model
source: assets/art/char/hero.glb
license: CC-BY-4.0
provenance:
  source_tier: original
  authors: [tester]
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


@pytest.mark.unit
class TestSkeletonDefsMerge:
    """meridian/skeleton@1 vocabulary $defs are merged into every content schema
    (contract ①/T1) the same way common.defs.yaml is — see load_schemas()."""

    def test_skeleton_defs_merged_into_schemas(self):
        from validate_content import load_schemas

        validators = load_schemas(SCHEMA_DIR)
        # any loaded validator's schema resolves the merged defs
        merged = validators["item"].schema["$defs"]
        assert set(merged["geosetRegion"]["enum"]) == {
            "head",
            "hands",
            "forearms",
            "torso",
            "waist",
            "hips_legs",
            "lower_legs",
            "feet",
        }
        assert "main_hand" in merged["attachSocket"]["enum"]
        assert merged["dyeChannel"]["enum"] == ["primary", "secondary", "accent"]

    def test_contentid_accepts_dye_and_appearance_segments(self):
        import re

        from validate_content import load_schemas

        defs = load_schemas(SCHEMA_DIR)["item"].schema["$defs"]
        pat = re.compile(defs["contentId"]["pattern"])
        assert pat.match("core:dye.russet")
        assert pat.match("core:appearance.ardent.male")


@pytest.mark.unit
class TestProvenanceLint:
    """L021/L022 — provenance completeness + license/origin policy (TD-09)."""

    def test_valid_sidecar_passes(self, tmp_path):
        res = run(tmp_path, {"tp/assets/art/hero.asset.yaml": ASSET})
        assert res.errors == []
        assert res.warnings == []

    def test_l021_missing_provenance_field_errors(self, tmp_path):
        # cc0 tier requires origin_url + license_verified_on. Exercise the lint
        # directly: the schema's conditional allOf would reject this shape first,
        # so a full-tree run never reaches L021 for schema-expressible gaps — the
        # lint is the layer that survives a loosened schema (Art SAD §3.2).
        from validate_content import check_provenance  # noqa: E402

        doc = {
            "license": "CC0-1.0",
            "provenance": {"source_tier": "cc0", "authors": ["t"]},
        }
        errors = check_provenance(doc, Path("x.asset.yaml"))
        assert any(e.startswith("L021") and "origin_url" in e for e in errors)

    def test_l021_ai_tier_missing_ai_block_errors(self, tmp_path):
        from validate_content import check_provenance  # noqa: E402

        doc = {
            "license": "CC-BY-4.0",
            "provenance": {
                "source_tier": "ai",
                "authors": ["t"],
                "origin_url": "https://example.com",
            },
        }
        errors = check_provenance(doc, Path("x.asset.yaml"))
        assert any(e.startswith("L021") and "ai." in e for e in errors)

    def test_l022_disallowed_license_errors(self, tmp_path):
        # A schema-legal-but-policy-banned license: bypass the schema enum by
        # asserting the lint fires on a permissive license the allowlist rejects.
        sidecar = ASSET.replace("license: CC-BY-4.0", "license: MIT")
        res = run(tmp_path, {"tp/assets/art/hero.asset.yaml": sidecar})
        # Schema enum also rejects MIT; both surface as errors — assert the policy
        # message is present regardless of ordering.
        assert any(e.startswith("L022") or "license" in e for e in res.errors)

    def test_l022_disallowed_license_via_lint_directly(self, tmp_path):
        from validate_content import check_provenance  # noqa: E402

        doc = {
            "license": "MIT",
            "provenance": {"source_tier": "original", "authors": ["t"]},
        }
        errors = check_provenance(doc, Path("x.asset.yaml"))
        assert any(e.startswith("L022") and "allowlist" in e for e in errors)

    def test_l022_engine_locked_origin_errors(self, tmp_path):
        from validate_content import check_provenance  # noqa: E402

        doc = {
            "license": "CC0-1.0",
            "provenance": {
                "source_tier": "cc0",
                "authors": ["t"],
                "origin_url": "https://quixel.com/megascans/rock01",
                "license_verified_on": "2026-01-01",
            },
        }
        errors = check_provenance(doc, Path("x.asset.yaml"))
        assert any(e.startswith("L022") and "engine-locked" in e for e in errors)

    def test_l022_engine_locked_origin_fails_full_tree(self, tmp_path):
        # Schema-valid cc0 sidecar (complete provenance) but the origin is an
        # engine-locked marketplace — only L022 catches this; proves it fires
        # through the full validate() path, not just the unit helper.
        sidecar = """\
        schema: meridian/asset@1
        id: tp:art.prop.rock
        class: prop
        source: assets/art/prop/rock.glb
        license: CC0-1.0
        provenance:
          source_tier: cc0
          authors: [tester]
          origin_url: https://quixel.com/megascans/rock01
          license_verified_on: "2026-01-01"
        """
        res = run(tmp_path, {"tp/assets/art/rock.asset.yaml": sidecar})
        assert "L022" in codes(res.errors)
        assert any("engine-locked" in e for e in res.errors)


@pytest.mark.unit
class TestBudgetLint:
    """L070/L071/L072 — declared budget vs Art PRD §2.1/§2.3/§2.4 class caps."""

    def test_within_budget_passes(self, tmp_path):
        sidecar = ASSET + "budget:\n  lod0_tris: 55000\n  texture_max_px: 2048\n"
        res = run(tmp_path, {"tp/assets/art/hero.asset.yaml": sidecar})
        assert res.errors == []

    def test_l070_over_tri_budget_errors(self, tmp_path):
        # character_model LOD0 ceiling is 60k (Art PRD §2.1); declare 80k.
        sidecar = ASSET + "budget:\n  lod0_tris: 80000\n"
        res = run(tmp_path, {"tp/assets/art/hero.asset.yaml": sidecar})
        assert "L070" in codes(res.errors)

    def test_l071_over_texture_budget_errors(self, tmp_path):
        # prop texture cap is 1024² (Art PRD §2.3); declare 4096 on a prop.
        sidecar = (
            ASSET.replace("class: character_model", "class: prop")
            .replace("id: tp:art.char.hero", "id: tp:art.prop.rock")
            .replace(
                "source: assets/art/char/hero.glb", "source: assets/art/prop/rock.glb"
            )
            + "budget:\n  texture_max_px: 4096\n"
        )
        res = run(tmp_path, {"tp/assets/art/rock.asset.yaml": sidecar})
        assert "L071" in codes(res.errors)

    def test_l072_over_material_set_budget_errors(self, tmp_path):
        # kit_piece cap is 12 material sets (Art PRD §2.3/§2.4); declare 20.
        sidecar = (
            ASSET.replace("class: character_model", "class: kit_piece")
            .replace("id: tp:art.char.hero", "id: tp:art.kit.wall")
            .replace(
                "source: assets/art/char/hero.glb", "source: assets/art/kit/wall.glb"
            )
            + "budget:\n  material_sets: 20\n"
        )
        res = run(tmp_path, {"tp/assets/art/wall.asset.yaml": sidecar})
        assert "L072" in codes(res.errors)

    def test_no_budget_block_is_inert(self, tmp_path):
        # Real content declares no budget block yet — must still pass.
        res = run(tmp_path, {"tp/assets/art/hero.asset.yaml": ASSET})
        assert res.errors == []


@pytest.mark.unit
class TestStemManifestLint:
    """L023 — a strudel-backed music_stem must ship a sibling render manifest."""

    # A schema-valid music_stem sidecar whose WAV `source` is rendered from a
    # Strudel pattern (declared in extra_sources). Individual tests decide whether
    # the sibling `<name>.render.yaml` manifest is present.
    STRUDEL_STEM = """\
    schema: meridian/asset@1
    id: tp:mus.z1.explore
    class: music_stem
    source: assets/audio/mus/explore.wav
    extra_sources:
      - assets/audio/mus/explore.strudel
    license: CC-BY-4.0
    provenance:
      source_tier: original
      authors: [tester]
    music:
      stem_set: mus.z1.explore_set
      layer: L1
      bpm: 120
      time_signature: "4/4"
      length_bars: 8
    loudness:
      lufs_integrated: -18
      true_peak_dbtp: -1.5
    """

    def test_l023_strudel_stem_without_manifest_errors(self, tmp_path):
        res = run(tmp_path, {"tp/assets/mus/z1_explore.asset.yaml": self.STRUDEL_STEM})
        assert "L023" in codes(res.errors)

    def test_l023_strudel_stem_with_manifest_passes(self, tmp_path):
        res = run(
            tmp_path,
            {
                "tp/assets/mus/z1_explore.asset.yaml": self.STRUDEL_STEM,
                "tp/assets/mus/z1_explore.render.yaml": "# strudel render manifest\n",
            },
        )
        assert "L023" not in codes(res.errors)

    def test_render_manifest_not_flagged_as_bad_filename(self, tmp_path):
        # A *.render.yaml manifest is an auxiliary strudel-render artifact, not a
        # content entity — discovery must skip it, so it never trips L001.
        manifest = (
            "schema: meridian/strudel-render@1\n"
            "pattern: assets/audio/mus/explore.strudel\n"
            "variant: 0\n"
            "tail_seconds: 4.0\n"
            "sample_banks: []\n"
        )
        res = run(
            tmp_path,
            {
                "tp/assets/mus/z1_explore.asset.yaml": self.STRUDEL_STEM,
                "tp/assets/mus/z1_explore.render.yaml": manifest,
            },
        )
        assert "L001" not in codes(res.errors)


@pytest.mark.integration
class TestRepoContent:
    def test_repo_content_validates_clean(self):
        res = validate(REPO / "content", SCHEMA_DIR, assets_mode="error")
        assert res.errors == [], res.errors
        assert res.warnings == [], res.warnings
