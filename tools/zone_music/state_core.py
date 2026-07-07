# SPDX-License-Identifier: Apache-2.0
"""ZoneMusicPlayer state machine + crossfade scheduler — the engine-free core.

This is the *pure logic* of the adaptive-music runtime (music SAD §2.1): the
explore/tension/combat/silence state machine, the vertical layer mix per state
(L1 never stops — SAD §2.2), the transition table (hysteresis, quantization,
fade), and the sample-domain crossfade schedule a transition produces. It touches
no Godot audio API, so it is unit-testable with plain pytest and is the reference
the GDScript `music_state_core.gd` mirrors 1:1.

Vertical layering (music PRD §2.1 / SAD §2.2):

    explore = L1 + L2          (bed + zone-motif melody)
    tension = L1 + L3          (bed + rhythmic ostinato)
    combat  = L1 + L3 + L4     (bed + ostinato + full percussion/brass)
    silence = rest (all layers fade toward the −60 dB floor; L1 held, not stopped)

Inaudible layers sit at `FLOOR_DB` (−60 dB), never stopped, so the synchronized
stack stays sample-locked — that is what makes transitions musical rather than
restarts.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from td11_music_timing.probe import ClockConfig

from .bar_clock import (
    QUANTIZE_BAR,
    QUANTIZE_BEAT,
    QUANTIZE_IMMEDIATE,
    QUANTIZE_LOOP_END,
    boundary_sample,
)

# --- states & layers -----------------------------------------------------
EXPLORE = "explore"
TENSION = "tension"
COMBAT = "combat"
SILENCE = "silence"
STATES = (EXPLORE, TENSION, COMBAT, SILENCE)

LAYERS = ("L1", "L2", "L3", "L4")

FULL_DB = 0.0
FLOOR_DB = -60.0  # inaudible-but-not-stopped floor (music SAD §2.2)

# Vertical mix per state: layer -> target gain (dB). L1 is always audible.
STATE_LAYER_GAINS: dict[str, dict[str, float]] = {
    EXPLORE: {"L1": FULL_DB, "L2": FULL_DB, "L3": FLOOR_DB, "L4": FLOOR_DB},
    TENSION: {"L1": FULL_DB, "L2": FLOOR_DB, "L3": FULL_DB, "L4": FLOOR_DB},
    COMBAT: {"L1": FULL_DB, "L2": FLOOR_DB, "L3": FULL_DB, "L4": FULL_DB},
    SILENCE: {"L1": FLOOR_DB, "L2": FLOOR_DB, "L3": FLOOR_DB, "L4": FLOOR_DB},
}


def layer_gains_db(state: str) -> dict[str, float]:
    """Target per-layer gain (dB) for `state` — the vertical mix (SAD §2.2)."""
    if state not in STATE_LAYER_GAINS:
        raise ValueError(f"unknown music state: {state!r}")
    return dict(STATE_LAYER_GAINS[state])


def director_state(
    combat_flag: bool,
    hostile_proximity: bool,
    boss_encounter: bool = False,
) -> str:
    """Reduce gameplay inputs to a target music state (the MusicDirector rule,
    music SAD §2.4). Combat/boss beats tension beats explore; the region says
    *what plays*, game state says *which layers*."""
    if combat_flag or boss_encounter:
        return COMBAT
    if hostile_proximity:
        return TENSION
    return EXPLORE


# --- transition table (music SAD §2.1) -----------------------------------
@dataclass(frozen=True)
class TransitionRule:
    """One (from -> to) row of the SAD §2.1 transition table."""

    quantize: str  # bar | beat | loop_end | immediate
    hysteresis_s: float = 0.0  # wall-clock delay before the flip is queued
    fade_bars: float = 0.0  # equal-power fade length in bars (music transitions)
    fade_ms: float = 0.0  # OR an absolute fade in ms (combat entry, ≤500 ms)
    stinger: str | None = None  # one-shot cue that masks the splice (§2.3)


# Keyed (from_state, to_state). Mirrors music SAD §2.1 exactly.
TRANSITION_TABLE: dict[tuple[str, str], TransitionRule] = {
    (EXPLORE, TENSION): TransitionRule(QUANTIZE_BAR, 0.0, fade_bars=1.0),
    (EXPLORE, COMBAT): TransitionRule(QUANTIZE_BEAT, 0.0, fade_ms=500.0,
                                      stinger="combat_enter"),
    (TENSION, COMBAT): TransitionRule(QUANTIZE_BEAT, 0.0, fade_ms=500.0,
                                      stinger="combat_enter"),
    (COMBAT, TENSION): TransitionRule(QUANTIZE_BAR, 4.0, fade_bars=2.0,
                                      stinger="combat_end"),
    (COMBAT, EXPLORE): TransitionRule(QUANTIZE_BAR, 4.0, fade_bars=2.0,
                                      stinger="combat_end"),
    (TENSION, EXPLORE): TransitionRule(QUANTIZE_BAR, 6.0, fade_bars=2.0),
    (EXPLORE, SILENCE): TransitionRule(QUANTIZE_LOOP_END, 0.0, fade_bars=2.0),
    (SILENCE, EXPLORE): TransitionRule(QUANTIZE_IMMEDIATE, 0.0, fade_bars=1.0),
    # Silence is overridden immediately by a combat/tension trigger (§2.1 row).
    (SILENCE, COMBAT): TransitionRule(QUANTIZE_IMMEDIATE, 0.0, fade_ms=500.0,
                                      stinger="combat_enter"),
    (SILENCE, TENSION): TransitionRule(QUANTIZE_IMMEDIATE, 0.0, fade_ms=500.0),
}


def rule_for(from_state: str, to_state: str) -> TransitionRule:
    """Transition rule for a state change. Unlisted pairs fall back to a plain
    next-bar, 2-bar equal-power fade (the §2.1 'any -> any set change' default)."""
    if from_state not in STATE_LAYER_GAINS:
        raise ValueError(f"unknown from_state: {from_state!r}")
    if to_state not in STATE_LAYER_GAINS:
        raise ValueError(f"unknown to_state: {to_state!r}")
    return TRANSITION_TABLE.get(
        (from_state, to_state), TransitionRule(QUANTIZE_BAR, 0.0, fade_bars=2.0)
    )


# --- crossfade schedule --------------------------------------------------
@dataclass(frozen=True)
class LayerRamp:
    """One per-layer equal-power gain ramp inside a crossfade."""

    layer: str
    start_db: float
    end_db: float
    begin_sample: int
    end_sample: int
    curve: str = "equal_power"

    @property
    def rising(self) -> bool:
        return self.end_db > self.start_db


@dataclass(frozen=True)
class CrossfadeSchedule:
    """The sample-domain plan for one state transition. This is what the
    GDScript player executes and what the TD-11 probe times against: the
    `boundary_sample` is the `predicted_boundary_sample` of a `TransitionEvent`."""

    from_state: str
    to_state: str
    request_sample: int
    boundary_sample: int
    fade_samples: int
    quantize: str
    ramps: tuple[LayerRamp, ...] = field(default_factory=tuple)
    stinger: str | None = None

    def ramp_for(self, layer: str) -> LayerRamp | None:
        for r in self.ramps:
            if r.layer == layer:
                return r
        return None


def fade_samples_for(rule: TransitionRule, cfg: ClockConfig) -> int:
    """Fade length in samples: an explicit ms fade (combat entry) wins, else
    the bar-count fade converted through the sample grid."""
    if rule.fade_ms > 0.0:
        return int(round(cfg.ms_to_samples(rule.fade_ms)))
    return int(round(rule.fade_bars * cfg.samples_per_bar))


def crossfade_schedule(
    from_state: str,
    to_state: str,
    cfg: ClockConfig,
    request_sample: int,
    length_bars: int | None = None,
) -> CrossfadeSchedule:
    """Compute the crossfade schedule for `from_state -> to_state` requested at
    `request_sample`. Only layers whose target gain changes get a ramp; the
    boundary is quantized per the §2.1 rule; the fade is equal-power."""
    rule = rule_for(from_state, to_state)
    boundary = boundary_sample(request_sample, cfg, rule.quantize, length_bars)
    fade = fade_samples_for(rule, cfg)

    from_gains = layer_gains_db(from_state)
    to_gains = layer_gains_db(to_state)
    ramps: list[LayerRamp] = []
    for layer in LAYERS:
        start = from_gains[layer]
        end = to_gains[layer]
        if start == end:
            continue
        ramps.append(
            LayerRamp(
                layer=layer,
                start_db=start,
                end_db=end,
                begin_sample=boundary,
                end_sample=boundary + fade,
            )
        )
    return CrossfadeSchedule(
        from_state=from_state,
        to_state=to_state,
        request_sample=int(request_sample),
        boundary_sample=boundary,
        fade_samples=fade,
        quantize=rule.quantize,
        ramps=tuple(ramps),
        stinger=rule.stinger,
    )
