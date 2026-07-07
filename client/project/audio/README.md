<!-- SPDX-License-Identifier: Apache-2.0 -->
# `client/project/audio` — ZoneMusicPlayer v1 (AUD-02, issue #144)

The client-side adaptive-music runtime (music [SAD §2](../../../docs/sad/music-sad.md)).
Given a zone id it plays that zone's music **set**; on a state change it
**crossfades** the vertical layer mix (explore / tension / combat / silence).

> ⚠️ **PLACEHOLDER AUDIO.** Per the owner ruling on #144, the *system* is built at
> M0 against **synthesized placeholder tones** — `placeholder_stream_factory.gd`
> procedurally generates one seamless-looping sine per stem. **This is not music
> and does not claim to be.** The real audio direction (#143) is deferred; when it
> lands, real `mus.*` stems resolve through the same ID hook path and the factory
> is unused in shipping. No audio asset is committed to the repo.

## Files

| File | Role |
|------|------|
| `zone_music_player.gd` | `ZoneMusicPlayer` node — the audio graph (`AudioStreamSynchronized` stem stack), bar/beat-quantized crossfades, shadow bar clock, timing-seam telemetry. |
| `music_state_core.gd` | Engine-free (`RefCounted`) state machine + transition table (SAD §2.1) + crossfade scheduler + sample-domain bar clock. 1:1 mirror of the Python reference. |
| `zone_track_map.gd` | Engine-free zone → set → stem resolution from `zone_music_config.json`. |
| `placeholder_stream_factory.gd` | **PLACEHOLDER** procedural tone generator (stand-in for the #148 ID→resource hook). |
| `zone_music_config.json` | Zone→set→stem mapping + musical metadata. Data, not code (SAD §6.1). |
| `zone_music_player_verify.gd` | Headless runtime verify (`SceneTree`, PASS/FAIL, exit 0/1). |
| `music_timing_probe.gd` | TD-11 timing probe — drives a live player, emits framed JSON for the `GodotTimingSource` (#145 harness). |

## Design (music SAD §2.1–2.4)

- **Vertical layering.** One `AudioStreamSynchronized` per set; all stems play
  sample-locked, per-stem volume is the mix, **L1 (bed) never stops** — that is
  what makes transitions musical. `explore = L1+L2`, `tension = L1+L3`,
  `combat = L1+L3+L4`, `silence = rest`.
- **Transitions.** Combat *entry* is next-**beat** quantized (≤500 ms, feels
  immediate) with a combat-entry stinger; combat *exit* has 4 s hysteresis and a
  2-bar next-**bar** equal-power fade. Full table in `music_state_core.gd`
  (`rule_for`), matching SAD §2.1.
- **One pending transition** (last-writer-wins). It fires when the sample-domain
  shadow clock crosses the quantized boundary — the poll granularity is exactly
  the error TD-11 measures.

## Zone→track mapping & the ID hook path (#148)

Content references stable **asset IDs only** (`core:mus.*`), never file paths
(music PRD §7). `zone_music_config.json` maps `zone → set → [stems]`; the runtime
datastore (IF-5 pack manifest, `PackEntry.resource`) resolves each asset ID to a
`res://meridian/...` resource. At M0 that resolver is the placeholder factory. The
**same** JSON is loaded by the Python reference (`tools/zone_music`) so the two
agree by construction.

## The TD-11 timing seam (#145 / #147)

`ZoneMusicPlayer` exposes the sample-domain seam the TD-11 harness measures
(music SAD §3.1): a **shadow bar clock** (`predicted_boundary`), per-stem
**gain-edge telemetry** (`drain_gain_edges`), per-stem **positions**
(`stem_positions`), and the last fired transition (`last_transition`).
`music_timing_probe.gd` drives a live player and prints the harness's
`TransitionEvent` / `DriftSample` records as framed JSON; the harness's
`GodotTimingSource` (`tools/td11_music_timing/probe.py`) shells out to it. #147
runs `--source godot` under the #111 bot fleet for the gate ruling.

## Running

```bash
# Engine-free logic (runs anywhere — no Godot, no audio device):
uv run pytest tests/test_zone_music.py -q

# Headless runtime verify (needs a Godot 4.7 binary; Dummy audio driver, no device):
godot --headless --path client/project --script res://audio/zone_music_player_verify.gd

# Real-audio TD-11 measurement through the seam (needs Godot 4.7):
GODOT_BIN=/path/to/godot PYTHONPATH=tools python3 -m td11_music_timing --source godot --out out/td11
```

**Audio confirmation needs a device** — the *sound* and the *feel* of the
crossfade are owner-confirmed; the headless checks prove the player instantiates,
loads the placeholder stack, and produces the correct schedule/state/timing.
