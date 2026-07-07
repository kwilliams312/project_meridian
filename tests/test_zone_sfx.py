# SPDX-License-Identifier: Apache-2.0
"""Tests for tools/zone_sfx — the engine-free SFX trigger core (#148).

Covers the "pure logic, testable where separable" slice the task calls for: the
gameplay/UI event -> sfx-id registry, the content-ID hook (asset id -> runtime
resource) shared with content-carried ids, and the category -> bus/group routing
(music SAD §2.5). The SFX counterpart of tests/test_zone_music.py (#144).
"""

import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

from zone_sfx import (  # noqa: E402
    SfxConfigError,
    SfxEventMap,
)

pytestmark = pytest.mark.unit


# --- config load / structure --------------------------------------------
def test_config_file_loads_and_parses():
    m = SfxEventMap.from_file()  # the real committed config
    assert m.placeholder is True
    # ~10 placeholder one-shots authored (issue #148: "~10 SFX").
    assert len(m.sfx_ids()) >= 10


def test_content_references_ids_not_paths():
    m = SfxEventMap.from_file()
    for sfx_id in m.sfx_ids():
        assert sfx_id.split(":", 1)[-1].startswith("sfx.")
        assert "/" not in sfx_id  # an ID, never a file path


# --- event -> sfx id resolution (music SAD §5) --------------------------
def test_ui_click_event_resolves_to_id():
    m = SfxEventMap.from_file()
    assert m.resolve_event("ui.click") == "core:sfx.ui.click"


def test_login_sting_event_resolves_to_id():
    m = SfxEventMap.from_file()
    assert m.resolve_event("login.sting") == "core:sfx.ui.login_sting"


def test_footstep_resolves_by_surface():
    m = SfxEventMap.from_file()
    # physics-material -> surface tag -> foley footstep variation group.
    for surface in ("grass", "stone", "dirt", "wood"):
        key = m.footstep_event(surface)
        assert m.resolve_event(key) == f"core:sfx.foley.footstep.{surface}"


def test_unknown_event_raises():
    m = SfxEventMap.from_file()
    with pytest.raises(KeyError, match="no SFX mapped"):
        m.resolve_event("footstep.lava")


# --- the ID hook path (#148) --------------------------------------------
def test_id_hook_path_resolver_is_injectable():
    # The #148 ID hook: asset id -> runtime resource, via an injected resolver.
    m = SfxEventMap.from_file(asset_resolver=lambda sid: f"res://meridian/{sid}")
    entry, resolved = m.resolved_sfx("core:sfx.ui.click")
    assert entry.sfx_id == "core:sfx.ui.click"
    assert resolved == "res://meridian/core:sfx.ui.click"


def test_event_and_id_hook_compose_end_to_end():
    m = SfxEventMap.from_file(asset_resolver=lambda sid: f"stream::{sid}")
    sfx_id = m.resolve_event("world.loot_pickup")
    _, resolved = m.resolved_sfx(sfx_id)
    assert resolved == "stream::core:sfx.world.loot_pickup"


def test_content_carried_id_uses_the_same_hook():
    # An ability's audio_visual.impact_sfx (core:sfx.combat.impact.pickaxe, from
    # /content) resolves through the SAME hook — no parallel path — even though
    # it is not pre-declared in sfx_config.json's `sfx` table.
    m = SfxEventMap.from_file(asset_resolver=lambda sid: f"res://meridian/{sid}")
    resolved = m.resolve_content_sfx("core:sfx.combat.impact.pickaxe")
    assert resolved == "res://meridian/core:sfx.combat.impact.pickaxe"


def test_content_hook_rejects_non_sfx_id():
    m = SfxEventMap.from_file()
    with pytest.raises(SfxConfigError, match="not a valid sfx"):
        m.resolve_content_sfx("core:art.vfx.impact.dust_small")


# --- category -> bus/group routing (music SAD §2.5) ---------------------
def test_ui_routes_to_ui_bus():
    m = SfxEventMap.from_file()
    assert m.bus_for("core:sfx.ui.click") == "UI"
    assert m.group_for("core:sfx.ui.click") == "ui"


def test_footstep_routes_to_foley_bus():
    m = SfxEventMap.from_file()
    assert m.bus_for("core:sfx.foley.footstep.grass") == "SFX_Foley"


def test_combat_routes_to_combat_bus():
    m = SfxEventMap.from_file()
    assert m.bus_for("core:sfx.combat.impact.generic") == "SFX_Combat"


def test_every_sfx_routes_to_a_known_bus():
    m = SfxEventMap.from_file()
    valid_buses = {"SFX_Combat", "SFX_Foley", "SFX_NPC", "SFX_World", "UI"}
    for sfx_id in m.sfx_ids():
        assert m.bus_for(sfx_id) in valid_buses


# --- structural validation (caught at load, like TLS-07) ----------------
def _base_cfg() -> dict:
    return {
        "categories": {"ui": {"bus": "UI", "group": "ui", "cap": 4}},
        "sfx": {"core:sfx.ui.click": {"category": "ui", "attenuation": "ui2d"}},
        "events": {"ui.click": "core:sfx.ui.click"},
    }


def test_event_referencing_unknown_sfx_is_rejected():
    bad = _base_cfg()
    bad["events"]["ui.oops"] = "core:sfx.ui.does_not_exist"
    with pytest.raises(SfxConfigError, match="unknown sfx id"):
        SfxEventMap(bad)


def test_sfx_with_unknown_category_is_rejected():
    bad = _base_cfg()
    bad["sfx"]["core:sfx.ui.click"]["category"] = "no.such.category"
    with pytest.raises(SfxConfigError, match="category"):
        SfxEventMap(bad)


def test_sfx_id_that_is_a_path_is_rejected():
    bad = _base_cfg()
    bad["sfx"]["assets/audio/sfx/click.wav"] = {
        "category": "ui",
        "attenuation": "ui2d",
    }
    with pytest.raises(SfxConfigError, match="not a valid sfx"):
        SfxEventMap(bad)


def test_bad_attenuation_is_rejected():
    bad = _base_cfg()
    bad["sfx"]["core:sfx.ui.click"]["attenuation"] = "enormous"
    with pytest.raises(SfxConfigError, match="attenuation"):
        SfxEventMap(bad)


def test_config_without_categories_is_rejected():
    bad = {"sfx": {}, "events": {}}
    with pytest.raises(SfxConfigError, match="no SFX categories"):
        SfxEventMap(bad)


def test_config_without_sfx_is_rejected():
    bad = {"categories": {"ui": {"bus": "UI", "group": "ui", "cap": 4}}}
    with pytest.raises(SfxConfigError, match="no SFX entries"):
        SfxEventMap(bad)
