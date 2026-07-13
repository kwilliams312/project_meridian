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
        schema: meridian/item@2
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
        schema: meridian/item@2
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


@pytest.mark.unit
class TestRestyleQuarantineLint:
    """L024 — source_tier != original cannot merge with restyle_status pending
    (issue #525; Art PRD §3.4, Art SAD §3.2 "tier != original => restyle_status:
    done", asset.schema.yaml's restyle_status comment)."""

    # A fully-valid, already-restyled ai-tier sidecar; individual tests mutate
    # source_tier / restyle_status. prompts_file is declared but its existence
    # (L025) is exercised separately — write the sibling file in build() so
    # these tests isolate L024 from L025 noise.
    AI_DONE = """\
    schema: meridian/asset@1
    id: tp:art.char.hero
    class: character_model
    source: assets/art/char/hero.glb
    license: CC-BY-4.0
    provenance:
      source_tier: ai
      authors: [tester]
      origin_url: https://api.meshy.ai/openapi/v2/text-to-3d/abc123
      ai:
        tool: meshy@meshy-5
        prompts_file: hero.prompts.yaml
    restyle_status: done
    """

    CC0_DONE = """\
    schema: meridian/asset@1
    id: tp:art.prop.rock
    class: prop
    source: assets/art/prop/rock.glb
    license: CC0-1.0
    provenance:
      source_tier: cc0
      authors: [tester]
      origin_url: https://polyhaven.com/a/rock_01
      license_verified_on: "2026-01-01"
    restyle_status: done
    """

    CC_BY_DONE = """\
    schema: meridian/asset@1
    id: tp:art.prop.plant
    class: prop
    source: assets/art/prop/plant.glb
    license: CC-BY-4.0
    provenance:
      source_tier: cc_by
      authors: [tester]
      origin_url: https://ambientcg.com/plant01
      attribution: "Plant01 by ambientCG"
      license_verified_on: "2026-01-01"
    restyle_status: done
    """

    def _tree(self, **extra):
        return {
            "tp/assets/art/hero.asset.yaml": self.AI_DONE,
            "tp/assets/art/hero.prompts.yaml": "# prompts\n",
            **extra,
        }

    def test_ai_tier_restyle_done_passes(self, tmp_path):
        res = run(tmp_path, self._tree())
        assert "L024" not in codes(res.errors)

    def test_l024_ai_tier_restyle_absent_errors(self, tmp_path):
        sidecar = self.AI_DONE.replace("restyle_status: done\n", "")
        res = run(
            tmp_path,
            {
                "tp/assets/art/hero.asset.yaml": sidecar,
                "tp/assets/art/hero.prompts.yaml": "# prompts\n",
            },
        )
        assert "L024" in codes(res.errors)

    def test_l024_ai_tier_restyle_pending_errors(self, tmp_path):
        sidecar = self.AI_DONE.replace(
            "restyle_status: done", "restyle_status: pending"
        )
        res = run(
            tmp_path,
            {
                "tp/assets/art/hero.asset.yaml": sidecar,
                "tp/assets/art/hero.prompts.yaml": "# prompts\n",
            },
        )
        assert "L024" in codes(res.errors)

    def test_l024_cc0_tier_restyle_pending_errors(self, tmp_path):
        sidecar = self.CC0_DONE.replace(
            "restyle_status: done", "restyle_status: pending"
        )
        res = run(tmp_path, {"tp/assets/art/rock.asset.yaml": sidecar})
        assert "L024" in codes(res.errors)

    def test_l024_cc_by_tier_restyle_absent_errors(self, tmp_path):
        sidecar = self.CC_BY_DONE.replace("restyle_status: done\n", "")
        res = run(tmp_path, {"tp/assets/art/plant.asset.yaml": sidecar})
        assert "L024" in codes(res.errors)

    def test_l024_original_tier_restyle_pending_is_inert(self, tmp_path):
        # original-tier art has no restyle step; an explicit pending/absent
        # restyle_status on it is not a policy violation.
        sidecar = ASSET + "restyle_status: pending\n"
        res = run(tmp_path, {"tp/assets/art/hero.asset.yaml": sidecar})
        assert "L024" not in codes(res.errors)

    def test_l024_lint_directly_absent_status(self):
        from validate_content import check_restyle_quarantine  # noqa: E402

        doc = {"provenance": {"source_tier": "ai", "authors": ["t"]}}
        errors = check_restyle_quarantine(doc, Path("x.asset.yaml"))
        assert any(e.startswith("L024") for e in errors)

    def test_l024_lint_directly_original_tier_is_inert(self):
        from validate_content import check_restyle_quarantine  # noqa: E402

        doc = {"provenance": {"source_tier": "original", "authors": ["t"]}}
        errors = check_restyle_quarantine(doc, Path("x.asset.yaml"))
        assert errors == []


@pytest.mark.unit
class TestPromptsFileLint:
    """L025 — provenance.ai.prompts_file must exist on disk (issue #464/#525)."""

    AI_DONE = TestRestyleQuarantineLint.AI_DONE
    CC0_DONE = TestRestyleQuarantineLint.CC0_DONE

    def test_l025_prompts_file_missing_errors(self, tmp_path):
        # prompts_file is declared but no sibling file is written to disk.
        res = run(tmp_path, {"tp/assets/art/hero.asset.yaml": self.AI_DONE})
        assert "L025" in codes(res.errors)

    def test_l025_prompts_file_present_passes(self, tmp_path):
        res = run(
            tmp_path,
            {
                "tp/assets/art/hero.asset.yaml": self.AI_DONE,
                "tp/assets/art/hero.prompts.yaml": "# prompts\n",
            },
        )
        assert "L025" not in codes(res.errors)

    def test_l025_non_ai_tier_is_inert(self, tmp_path):
        # cc0 tier has no provenance.ai block at all — nothing to check.
        res = run(tmp_path, {"tp/assets/art/rock.asset.yaml": self.CC0_DONE})
        assert "L025" not in codes(res.errors)

    def test_l025_lint_directly_missing_file(self, tmp_path):
        from validate_content import check_prompts_file  # noqa: E402

        sidecar_dir = tmp_path / "tp" / "assets" / "art"
        sidecar_dir.mkdir(parents=True)
        doc = {
            "provenance": {
                "source_tier": "ai",
                "authors": ["t"],
                "ai": {"tool": "meshy@meshy-5", "prompts_file": "hero.prompts.yaml"},
            }
        }
        errors = check_prompts_file(
            doc, sidecar_dir / "hero.asset.yaml", Path("x.asset.yaml")
        )
        assert any(e.startswith("L025") for e in errors)

    def test_l025_lint_directly_present_file(self, tmp_path):
        from validate_content import check_prompts_file  # noqa: E402

        sidecar_dir = tmp_path / "tp" / "assets" / "art"
        sidecar_dir.mkdir(parents=True)
        (sidecar_dir / "hero.prompts.yaml").write_text("# prompts\n", encoding="utf-8")
        doc = {
            "provenance": {
                "source_tier": "ai",
                "authors": ["t"],
                "ai": {"tool": "meshy@meshy-5", "prompts_file": "hero.prompts.yaml"},
            }
        }
        errors = check_prompts_file(
            doc, sidecar_dir / "hero.asset.yaml", Path("x.asset.yaml")
        )
        assert errors == []


@pytest.mark.unit
class TestItem2Worn:
    """meridian/item@2 — visual.worn modular-gear contract + L080/L081 (contract ①/T2).

    One valid weapon fixture; each negative case copies it and mutates one field,
    per the one-negative-fixture-per-lint convention (Tools PRD §11.1)."""

    # A fully-valid item@2 weapon: worn with a single model + attach (no hides —
    # attach XOR skinned). visual.icon retained. assets_mode defaults to ignore so
    # the art refs need no sidecar in these unit fixtures.
    WORN_OK_WEAPON = """\
    schema: meridian/item@2
    id: tp:item.sword
    name: Sword
    item_class: weapon
    slot: main_hand
    rarity: common
    weapon: { damage: { min: 1, max: 2 }, speed_ms: 2000 }
    visual:
      icon: art.ui.icon.sword
      worn:
        models: [{ model: art.weapon.tp.sword, mirror: none }]
        attach: { socket: main_hand, sheath_socket: hip_l }
    """

    # A valid item@2 visible-slot armor: worn with skinned models + hides, NO attach.
    WORN_OK_ARMOR = """\
    schema: meridian/item@2
    id: tp:item.hauberk
    name: Hauberk
    item_class: armor
    slot: chest
    rarity: common
    visual:
      icon: art.ui.icon.hauberk
      worn:
        models: [{ model: art.armor.tp.hauberk, mirror: none }]
        hides: [torso, forearms]
        dye_channels: [primary, secondary]
    """

    def test_item2_weapon_with_attach_passes(self, tmp_path):
        res = run(tmp_path, {"tp/items/sword.item.yaml": self.WORN_OK_WEAPON})
        assert res.errors == []

    def test_item2_visible_armor_with_worn_passes(self, tmp_path):
        res = run(tmp_path, {"tp/items/hauberk.item.yaml": self.WORN_OK_ARMOR})
        assert res.errors == []

    def test_item2_armor_visible_slot_without_worn_fails_L080(self, tmp_path):
        # Drop the worn block entirely from a visible-slot armor.
        armor = (
            "schema: meridian/item@2\n"
            "id: tp:item.hauberk\n"
            "name: Hauberk\n"
            "item_class: armor\n"
            "slot: chest\n"
            "rarity: common\n"
            "visual:\n"
            "  icon: art.ui.icon.hauberk\n"
        )
        res = run(tmp_path, {"tp/items/hauberk.item.yaml": armor})
        assert "L080" in codes(res.errors)

    def test_item2_ring_with_worn_fails_L080(self, tmp_path):
        # Invisible slot (finger) must NOT declare worn.
        ring = self.WORN_OK_ARMOR.replace("slot: chest", "slot: finger").replace(
            "        hides: [torso, forearms]\n", ""
        )
        res = run(tmp_path, {"tp/items/ring.item.yaml": ring})
        assert "L080" in codes(res.errors)

    def test_item2_weapon_without_attach_fails_L081(self, tmp_path):
        # Weapon worn without attach → L081 (attach REQUIRED for weapons).
        weapon = self.WORN_OK_WEAPON.replace(
            "        attach: { socket: main_hand, sheath_socket: hip_l }\n", ""
        )
        res = run(tmp_path, {"tp/items/sword.item.yaml": weapon})
        assert "L081" in codes(res.errors)

    def test_item2_weapon_with_hides_fails_L081(self, tmp_path):
        # Weapon worn with hides → L081 (attach XOR skinned; weapons never hide geosets).
        weapon = self.WORN_OK_WEAPON.replace(
            "        attach: { socket: main_hand, sheath_socket: hip_l }\n",
            "        hides: [torso]\n"
            "        attach: { socket: main_hand, sheath_socket: hip_l }\n",
        )
        res = run(tmp_path, {"tp/items/sword.item.yaml": weapon})
        assert "L081" in codes(res.errors)

    def test_item2_armor_with_attach_fails_L081(self, tmp_path):
        # Armor worn with attach → L081 (attach FORBIDDEN for armor).
        armor = self.WORN_OK_ARMOR.replace(
            "        hides: [torso, forearms]\n",
            "        attach: { socket: back }\n",
        )
        res = run(tmp_path, {"tp/items/hauberk.item.yaml": armor})
        assert "L081" in codes(res.errors)

    def test_item2_non_shield_armor_subclass_with_attach_still_fails_L081(
        self, tmp_path
    ):
        # Regression (issue #460): a non-shield subclass on armor keeps the
        # original rule — attach stays FORBIDDEN. Only subclass:shield is exempt.
        armor = self.WORN_OK_ARMOR.replace(
            "    item_class: armor\n",
            "    item_class: armor\n    subclass: plate\n",
        ).replace(
            "        hides: [torso, forearms]\n",
            "        attach: { socket: back }\n",
        )
        res = run(tmp_path, {"tp/items/hauberk.item.yaml": armor})
        assert "L081" in codes(res.errors)

    # A valid item@2 shield: armor + subclass shield is bone-attached like a
    # weapon (issue #460) — attach REQUIRED, hides FORBIDDEN, same as a weapon,
    # even though it lives under item_class: armor.
    WORN_OK_SHIELD = """\
    schema: meridian/item@2
    id: tp:item.buckler
    name: Buckler
    item_class: armor
    subclass: shield
    slot: off_hand
    rarity: common
    visual:
      icon: art.ui.icon.buckler
      worn:
        models: [{ model: art.armor.tp.buckler, mirror: none }]
        attach: { socket: off_hand }
    """

    def test_item2_shield_with_attach_passes(self, tmp_path):
        res = run(tmp_path, {"tp/items/buckler.item.yaml": self.WORN_OK_SHIELD})
        assert res.errors == []

    def test_item2_shield_without_attach_fails_L081(self, tmp_path):
        # Shield worn without attach → L081 (shields follow the weapon path:
        # attach is REQUIRED, issue #460).
        shield = self.WORN_OK_SHIELD.replace(
            "        attach: { socket: off_hand }\n", ""
        )
        res = run(tmp_path, {"tp/items/buckler.item.yaml": shield})
        assert "L081" in codes(res.errors)

    def test_item2_shield_with_hides_fails_L081(self, tmp_path):
        # Shield worn with hides → L081 (attach XOR skinned; a shield covers no
        # body region, so hides is FORBIDDEN too, issue #460).
        shield = self.WORN_OK_SHIELD.replace(
            "        attach: { socket: off_hand }\n",
            "        hides: [torso]\n        attach: { socket: off_hand }\n",
        )
        res = run(tmp_path, {"tp/items/buckler.item.yaml": shield})
        assert "L081" in codes(res.errors)

    def test_item2_non_equippable_with_worn_fails_L080(self, tmp_path):
        # A non-weapon/armor item (quest) must not carry worn.
        quest_item = (
            "schema: meridian/item@2\n"
            "id: tp:item.totem\n"
            "name: Totem\n"
            "item_class: quest\n"
            "rarity: common\n"
            "visual:\n"
            "  icon: art.ui.icon.totem\n"
            "  worn:\n"
            "    models: [{ model: art.weapon.tp.totem }]\n"
        )
        res = run(tmp_path, {"tp/items/totem.item.yaml": quest_item})
        assert "L080" in codes(res.errors)

    def test_item2_unknown_geoset_region_fails_schema(self, tmp_path):
        # hides enum comes from skeleton.defs geosetRegion via $ref.
        armor = self.WORN_OK_ARMOR.replace(
            "        hides: [torso, forearms]\n", "        hides: [elbow]\n"
        )
        res = run(tmp_path, {"tp/items/hauberk.item.yaml": armor})
        assert "SCHEMA" in codes(res.errors)

    def test_item2_race_override_unknown_race_fails_schema(self, tmp_path):
        # race_overrides keys are constrained to skeleton.defs raceName via propertyNames.
        armor = self.WORN_OK_ARMOR.replace(
            "        dye_channels: [primary, secondary]\n",
            "        race_overrides:\n"
            "          klingon:\n"
            "            models: [{ model: art.armor.tp.hauberk_klingon }]\n",
        )
        res = run(tmp_path, {"tp/items/hauberk.item.yaml": armor})
        assert "SCHEMA" in codes(res.errors)

    def test_item1_envelope_now_rejected(self, tmp_path):
        # No back-compat window: an item@1 file is a hard L001 (expected item@2).
        legacy = (
            "schema: meridian/item@1\n"
            "id: tp:item.old\n"
            "name: Old\n"
            "item_class: quest\n"
            "rarity: common\n"
            "visual:\n"
            "  icon: art.ui.icon.old\n"
        )
        res = run(tmp_path, {"tp/items/old.item.yaml": legacy})
        assert "L001" in codes(res.errors)


@pytest.mark.unit
class TestWardenKitItems:
    """The six committed Warden's Kit item@2 files (story #569) — armor worn blocks
    pass L080/L081 (visible-slot armor: worn present, hides valid, no attach)."""

    EXPECTED_SLOTS = {"head", "shoulders", "chest", "hands", "legs", "feet"}
    WARDEN_ITEMS = sorted(
        (REPO / "content" / "core" / "items") / f"warden_{s}.item.yaml"
        for s in EXPECTED_SLOTS
    )

    def test_all_six_kit_files_present(self):
        assert all(p.is_file() for p in self.WARDEN_ITEMS)
        assert len(self.WARDEN_ITEMS) == 6

    @pytest.mark.parametrize("path", WARDEN_ITEMS, ids=lambda p: p.stem)
    def test_kit_item_passes_worn_lints(self, path):
        import yaml
        from validate_content import check_worn

        doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        assert doc["item_class"] == "armor"
        assert doc["visual"]["worn"]["dye_channels"] == ["primary", "secondary"]
        # No attach (armor skins onto the body); hides present.
        assert "attach" not in doc["visual"]["worn"]
        assert doc["visual"]["worn"]["hides"]
        errors = check_worn(doc, path.name)
        assert errors == [], errors


@pytest.mark.unit
class TestAppearanceCatalog:
    """meridian/appearance_catalog@1 — per race/sex customization catalog +
    L082/L083 (contract ①/T3). One valid fixture; each negative case copies it
    and mutates one field, per the one-negative-fixture-per-lint convention
    (Tools PRD §11.1)."""

    # A fully-valid catalog: race ardent, 1 hair/1 face/1 skin preset, no morphs.
    CATALOG_OK = """\
    schema: meridian/appearance_catalog@1
    id: tp:appearance.ardent.male
    race: ardent
    sex: male
    skeleton: art.char.ardent.male.skeleton
    body_model: art.char.ardent.male.base
    presets:
      hair: [{ id: 1, model: art.char.ardent.male.hair_short }]
      face: [{ id: 1, texture: art.char.ardent.male.face_a }]
      skin: [{ id: 1, palette: art.char.ardent.male.skin_pale }]
    morphs: []
    """

    def test_catalog_valid_passes(self, tmp_path):
        res = run(
            tmp_path, {"tp/appearance/ardent_male.appearance.yaml": self.CATALOG_OK}
        )
        assert res.errors == []

    def test_l082_duplicate_preset_id(self, tmp_path):
        # Two hair presets share id 1 — L082 (preset ids unique per preset list).
        catalog = self.CATALOG_OK.replace(
            "  hair: [{ id: 1, model: art.char.ardent.male.hair_short }]\n",
            "  hair: [{ id: 1, model: art.char.ardent.male.hair_short }, "
            "{ id: 1, model: art.char.ardent.male.hair_long }]\n",
        )
        res = run(tmp_path, {"tp/appearance/ardent_male.appearance.yaml": catalog})
        assert "L082" in codes(res.errors)

    def test_l083_duplicate_race_sex_catalog(self, tmp_path):
        # Two catalog files both declare race=ardent sex=male — L083 (one catalog
        # per race/sex across the tree). Different ids so L010 does not also fire.
        second = self.CATALOG_OK.replace(
            "id: tp:appearance.ardent.male", "id: tp:appearance.ardent.male_dup"
        )
        res = run(
            tmp_path,
            {
                "tp/appearance/ardent_male.appearance.yaml": self.CATALOG_OK,
                "tp/appearance/ardent_male_2.appearance.yaml": second,
            },
        )
        assert "L083" in codes(res.errors)

    def test_preset_id_zero_fails_schema(self, tmp_path):
        # Preset ids are `minimum: 1` — 0 is out of range.
        catalog = self.CATALOG_OK.replace(
            "{ id: 1, model: art.char.ardent.male.hair_short }",
            "{ id: 0, model: art.char.ardent.male.hair_short }",
        )
        res = run(tmp_path, {"tp/appearance/ardent_male.appearance.yaml": catalog})
        assert "SCHEMA" in codes(res.errors)

    def test_unknown_race_fails_schema(self, tmp_path):
        # race must be one of the skeleton.defs raceName roster ids.
        catalog = self.CATALOG_OK.replace("race: ardent", "race: elf")
        res = run(tmp_path, {"tp/appearance/ardent_male.appearance.yaml": catalog})
        assert "SCHEMA" in codes(res.errors)

    def test_more_than_two_morphs_fails_schema(self, tmp_path):
        # morphs is capped at 2 entries (A-03/D-32 crowd-render budget, §2.5).
        catalog = self.CATALOG_OK.replace(
            "morphs: []",
            "morphs:\n"
            "      - { id: 1, name: Brow Height, blendshape: brow_height, min: 0, max: 1 }\n"
            "      - { id: 2, name: Jaw Width, blendshape: jaw_width, min: 0, max: 1 }\n"
            "      - { id: 3, name: Nose Size, blendshape: nose_size, min: 0, max: 1 }",
        )
        res = run(tmp_path, {"tp/appearance/ardent_male.appearance.yaml": catalog})
        assert "SCHEMA" in codes(res.errors)


@pytest.mark.unit
class TestDye:
    """meridian/dye@1 — curated dye palette (contract ① §6). One valid fixture;
    each negative case copies it and mutates one field."""

    DYE_OK = """\
    schema: meridian/dye@1
    id: tp:dye.russet
    name: Russet
    color: "#8a4b2d"
    rarity: common
    """

    def test_dye_valid_passes(self, tmp_path):
        res = run(tmp_path, {"tp/dyes/russet.dye.yaml": self.DYE_OK})
        assert res.errors == []

    def test_bad_hex_color_fails_schema(self, tmp_path):
        dye = self.DYE_OK.replace('color: "#8a4b2d"', "color: red")
        res = run(tmp_path, {"tp/dyes/russet.dye.yaml": dye})
        assert "SCHEMA" in codes(res.errors)

    def test_missing_rarity_fails_schema(self, tmp_path):
        dye = self.DYE_OK.replace("rarity: common\n", "")
        res = run(tmp_path, {"tp/dyes/russet.dye.yaml": dye})
        assert "SCHEMA" in codes(res.errors)


@pytest.mark.unit
class TestAbilityEffectPalette:
    """Story #653 (SP1.3) — the extended Tier-1 effect-primitive palette.

    Every new kind must validate on a well-formed sample, and a malformed effect
    (bad enum, missing required field, unknown property, bad attribute ref, or
    too many effects) must be rejected with a SCHEMA error. The pre-existing
    damage/heal/aura/threat kinds must keep validating unchanged.
    """

    # An NPC the `summon` sample can resolve against (L011).
    SUMMON_NPC = NPC.replace("id: tp:npc.dummy", "id: tp:npc.imp")

    @staticmethod
    def _ability(effects_block: str) -> str:
        """Wrap an `effects:` YAML block into a complete, otherwise-valid ability."""
        return (
            "schema: meridian/ability@1\n"
            "id: tp:ability.sample\n"
            "name: Sample\n"
            "target: enemy\n"
            "school: fire\n"
            "effects:\n" + effects_block
        )

    def _run_ability(self, tmp_path, effects_block, extra=None):
        files = {"tp/abilities/sample.ability.yaml": self._ability(effects_block)}
        if extra:
            files.update(extra)
        return run(tmp_path, files)

    # --- positive: each new kind validates -------------------------------

    def test_dot_valid(self, tmp_path):
        res = self._run_ability(
            tmp_path,
            "  - kind: dot\n"
            "    amount: { min: 3, max: 5 }\n"
            "    duration_ms: 6000\n"
            "    tick_ms: 2000\n",
        )
        assert res.errors == []

    def test_hot_valid(self, tmp_path):
        res = self._run_ability(
            tmp_path,
            "  - kind: hot\n"
            "    amount: { min: 4, max: 6 }\n"
            "    duration_ms: 9000\n"
            "    tick_ms: 3000\n"
            "    max_stacks: 3\n",
        )
        assert res.errors == []

    def test_buff_valid(self, tmp_path):
        res = self._run_ability(
            tmp_path,
            "  - kind: buff\n"
            "    attribute: core:attribute.strength\n"
            "    amount: 10\n"
            "    modifier: flat\n"
            "    duration_ms: 30000\n",
        )
        assert res.errors == []

    def test_debuff_valid(self, tmp_path):
        res = self._run_ability(
            tmp_path,
            "  - kind: debuff\n"
            "    attribute: core:attribute.agility\n"
            "    amount: -15\n"
            "    modifier: percent\n"
            "    duration_ms: 12000\n",
        )
        assert res.errors == []

    def test_shield_valid(self, tmp_path):
        res = self._run_ability(
            tmp_path,
            "  - kind: shield\n"
            "    amount: { min: 200, max: 200 }\n"
            "    duration_ms: 10000\n",
        )
        assert res.errors == []

    def test_cc_valid(self, tmp_path):
        res = self._run_ability(
            tmp_path,
            "  - kind: cc\n    type: stun\n    duration_ms: 4000\n",
        )
        assert res.errors == []

    def test_resource_valid(self, tmp_path):
        res = self._run_ability(
            tmp_path,
            "  - kind: resource\n"
            "    pool: mana\n"
            "    operation: drain\n"
            "    amount: 300\n",
        )
        assert res.errors == []

    def test_movement_valid(self, tmp_path):
        res = self._run_ability(
            tmp_path,
            "  - kind: movement\n    motion: knockback\n    distance_m: 8\n",
        )
        assert res.errors == []

    def test_summon_valid(self, tmp_path):
        res = self._run_ability(
            tmp_path,
            "  - kind: summon\n    npc: npc.imp\n    count: 2\n    duration_ms: 60000\n",
            extra={"tp/npcs/imp.npc.yaml": self.SUMMON_NPC},
        )
        assert res.errors == []

    def test_existing_kinds_still_validate(self, tmp_path):
        # Regression: damage + aura (with periodic + stat_mods) + threat unchanged.
        res = self._run_ability(
            tmp_path,
            "  - kind: damage\n    amount: { min: 8, max: 13 }\n    coefficient: 0.5\n"
            "  - kind: aura\n    duration_ms: 9000\n"
            "    periodic: { kind: damage, amount: { min: 2, max: 3 }, tick_ms: 3000 }\n"
            "    stat_mods: [{ stat: strength, amount: 5 }]\n"
            "  - kind: threat\n    amount: 100\n",
        )
        assert res.errors == []

    def test_five_effect_kit_valid_after_maxitems_bump(self, tmp_path):
        # A signature ability chaining 5 primitives — proves the maxItems bump.
        res = self._run_ability(
            tmp_path,
            "  - kind: damage\n    amount: { min: 10, max: 15 }\n"
            "  - kind: dot\n    amount: { min: 2, max: 3 }\n    duration_ms: 6000\n    tick_ms: 2000\n"
            "  - kind: debuff\n    attribute: core:attribute.stamina\n    amount: -10\n    modifier: flat\n    duration_ms: 8000\n"
            "  - kind: cc\n    type: stun\n    duration_ms: 2000\n"
            "  - kind: movement\n    motion: knockback\n    distance_m: 6\n",
        )
        assert res.errors == []

    # --- negative: malformed effects are rejected ------------------------

    def test_cc_bad_type_rejected(self, tmp_path):
        res = self._run_ability(
            tmp_path, "  - kind: cc\n    type: freeze\n    duration_ms: 4000\n"
        )
        assert "SCHEMA" in codes(res.errors)

    def test_cc_missing_duration_rejected(self, tmp_path):
        res = self._run_ability(tmp_path, "  - kind: cc\n    type: stun\n")
        assert "SCHEMA" in codes(res.errors)

    def test_buff_missing_attribute_rejected(self, tmp_path):
        res = self._run_ability(
            tmp_path,
            "  - kind: buff\n    amount: 10\n    modifier: flat\n    duration_ms: 30000\n",
        )
        assert "SCHEMA" in codes(res.errors)

    def test_buff_bad_attribute_ref_rejected(self, tmp_path):
        # `strength` is not a namespaced attribute id (needs the `attribute.` type).
        res = self._run_ability(
            tmp_path,
            "  - kind: buff\n    attribute: strength\n    amount: 10\n"
            "    modifier: flat\n    duration_ms: 30000\n",
        )
        assert "SCHEMA" in codes(res.errors)

    def test_resource_bad_operation_rejected(self, tmp_path):
        res = self._run_ability(
            tmp_path,
            "  - kind: resource\n    pool: mana\n    operation: steal\n    amount: 300\n",
        )
        assert "SCHEMA" in codes(res.errors)

    def test_resource_bad_pool_rejected(self, tmp_path):
        res = self._run_ability(
            tmp_path,
            "  - kind: resource\n    pool: focus\n    operation: drain\n    amount: 300\n",
        )
        assert "SCHEMA" in codes(res.errors)

    def test_movement_bad_motion_rejected(self, tmp_path):
        res = self._run_ability(
            tmp_path, "  - kind: movement\n    motion: teleport\n    distance_m: 8\n"
        )
        assert "SCHEMA" in codes(res.errors)

    def test_dot_missing_tick_rejected(self, tmp_path):
        res = self._run_ability(
            tmp_path,
            "  - kind: dot\n    amount: { min: 3, max: 5 }\n    duration_ms: 6000\n",
        )
        assert "SCHEMA" in codes(res.errors)

    def test_unknown_property_rejected(self, tmp_path):
        res = self._run_ability(
            tmp_path,
            "  - kind: shield\n    amount: { min: 1, max: 1 }\n"
            "    duration_ms: 1000\n    bogus_field: 1\n",
        )
        assert "SCHEMA" in codes(res.errors)

    def test_unknown_kind_rejected(self, tmp_path):
        res = self._run_ability(tmp_path, "  - kind: nonsense\n    amount: 1\n")
        assert "SCHEMA" in codes(res.errors)

    def test_too_many_effects_rejected(self, tmp_path):
        one = "  - kind: cc\n    type: stun\n    duration_ms: 1000\n"
        res = self._run_ability(tmp_path, one * 7)
        assert "SCHEMA" in codes(res.errors)


@pytest.mark.integration
class TestRepoContent:
    def test_repo_content_validates_clean(self):
        res = validate(REPO / "content", SCHEMA_DIR, assets_mode="error")
        assert res.errors == [], res.errors
        assert res.warnings == [], res.warnings
