# PRD — Music / Audio Track

**Project:** Project Meridian (open-source WoW-style MMORPG)
**Track:** Music & Audio
**Version:** 0.5 — 2026-07-05 (v0.5: reviewed against Baseline v0.6 / D-29 (OPS-05 telemetry) — no audio deliverables. v0.4: reviewed against Baseline v0.5 / D-28 (macOS client) — no audio impact: the runtime is platform-neutral Godot audio; Ogg/WAV assets and the voice manager are identical on Metal-backend builds. v0.3: reviewed against Baseline v0.4 — CHR-05 mount audio added to §6/§9 (a Music ● since Baseline v0.2, never claimed); sharded-realm changes have no audio impact. v0.2: revised for the v0.3 engine pivot UE5 → Godot 4.6: adaptive music re-platformed from MetaSounds onto `AudioStreamInteractive`/`AudioStreamSynchronized` per TD-11 v0.3; middleware fallback replaced by a custom GDExtension mixer; runtime format now Ogg Vorbis.)
**Baseline:** [Game Design Baseline v0.6](../00-GAME-DESIGN-BASELINE.md). All feature IDs (AUD-01/02/03, etc.), milestone names (M0–M4), and technical decisions (TD-01..TD-12) referenced here are defined there and are binding.
**Primary owner features:** AUD-01 (SFX framework), AUD-02 (adaptive zone music), AUD-03 (ambient beds & emitters).

---

## 1. Overview & audio direction

### 1.1 Musical identity

Project Meridian's score is **orchestral-hybrid**: a core of orchestral instrumentation (strings, brass, woodwinds, choir) blended with folk/ethnic solo instruments and light synth texture for otherworldly zones. This matches the art direction (TD-10, stylized-realistic "painterly PBR") — music should feel painted, warm, and melodic rather than photoreal/cinematic-gritty.

Structural principles:

- **Thematic motifs.** One 4–8 bar **main theme** (login/character-select, CHR-01) established at M0/M1, plus a **motif per playable race** (2 races by M3) and a **motif per zone** (4 zones by M3). Zone scores quote race motifs where lore overlaps; combat layers quote the zone motif so adaptive transitions feel related, not swapped.
- **Melody-forward exploration, texture-forward tension.** Explore layers carry the tune; tension/combat layers add rhythm and density on top of the same harmonic bed (this is what makes vertical layering work — see §2).
- **Restraint.** WoW-style zones let music breathe: music regions target ~60–70% musical coverage with ambient-only gaps, controlled by cooldown/retrigger rules (§2.2), never wall-to-wall looping.

### 1.2 What "WoW-quality-or-better audio" means (testable definition)

1. Every zone has a distinct, hummable identity — a blind A/B listener can match zone screenshots to zone tracks.
2. Music state follows gameplay: entering combat, a dungeon boss room, or a night cycle audibly changes the score within 1 musical bar (AUD-02, §2).
3. No audible loop seams, no clipping, no >3 dB loudness jumps between adjacent assets (loudness standards §5.4).
4. SFX feedback is instant and legible: every player-facing combat event (CMB-01/03/04) has a sound, and 50 players fighting on screen stays intelligible under voice limits (§3.4) on min-spec hardware (TD-03).
5. All of it authored through shipped tools (Pillar 1): music regions and ambience volumes are placed in the Forge zone editor (TLS-02, a Godot editor plugin per TD-08), and content files reference audio by stable asset ID (§5.3 of baseline; §7 here).

### 1.3 M0 deliverable: the Audio Direction Doc

Per the baseline M0 scope ("Music: audio direction doc, pipeline proven with 1 adaptive zone track in Godot's interactive-music system"), the M0 deliverables of this track are:

- **`docs/audio-direction.md`** — the audio bible: musical identity, instrumentation palette per zone archetype, motif sheet (notated main theme + race motif sketches), SFX aesthetic rules (e.g. "impacts are woody/organic, UI is glassy/soft"), loudness standards, naming/ID conventions, sourcing & provenance policy (§5).
- **One adaptive zone track** (explore/tension/combat stems) running in the `ZoneMusicPlayer` runtime (§2.4) in the M0 test map, state-switched by a debug trigger — the pipeline proof, and the evidence for the TD-11 decision gate (§6, IT-M0 in §8).

---

## 2. Adaptive music system design (AUD-02)

Per TD-11 v0.3: Godot's interactive audio — `AudioStreamInteractive` (clip graph with per-transition rules) + `AudioStreamSynchronized` (sample-synchronized parallel stems), Godot 4.3+ — with a custom C++ GDExtension stem mixer as fallback if bar-quantized transitions prove insufficient. Decision gate: end of M0. Adaptive layered music per zone with **explore / tension / combat** layers.

### 2.1 Layer/stem architecture

Each zone music **set** is a group of tempo- and key-locked stems recorded against a shared click/grid:

| Layer | Stems | Content | Active when |
|-------|-------|---------|-------------|
| L1 Explore-base | 1–2 | Harmonic bed, pads, low strings | Always (when set is playing) |
| L2 Explore-melody | 1–2 | Zone motif: solo instrument, counter-melody | Explore state, faded out under tension |
| L3 Tension | 1 | Rhythmic ostinato, low percussion, tremolo strings | Hostile mob in aggro-warning range; elite/named nearby |
| L4 Combat | 1–2 | Full percussion, brass, driving rhythm | Player in combat (server combat flag, CMB-01) |

- **Per zone set: 5–7 stems**, each a seamless loop of 64–128 bars (~2–4 min at 80–110 BPM), all identical length so bar positions align.
- States are **vertical mixes** of stems: Explore = L1+L2; Tension = L1+L3; Combat = L1+L3+L4. L1 never stops, which is what makes transitions musical.
- Dungeon/boss sets (GRP-02, M2) reuse the same architecture with a hotter baseline: dungeon-explore already includes light tension percussion; a **boss state** adds a dedicated L4-boss stem and entry stinger.

### 2.2 Transition rules

- **Bar-quantized crossfades.** State changes are queued and executed on the next bar line, using `AudioStreamInteractive` transition rules set to next-bar timing (BPM + time signature/bar length are asset metadata in the content DB, mirrored into the stream's beat properties at import). Combat *entry* uses next-beat timing so it feels immediate: beat-quantized fade-in ≤ 500 ms. Fade-outs are 1–2 bars, equal-power.
- **Stingers.** Short (1–2 s) one-shot hits mask transitions: combat-entry stinger, victory/combat-end stinger, discovery stinger (WLD-03 POI discovery), level-up flourish, death sting (CMB-03). Stingers are pitch/key-tagged per set so they land in key.
- **Hysteresis.** Combat exit requires 4 s out of combat before the 2-bar fade back to explore — prevents thrash on chain pulls. Tension has 6 s hysteresis.
- **Silence scheduling.** After a full loop pass in explore state, the set rests for a randomized 60–180 s (ambient beds only, §4), then re-enters at a random valid section marker. Combat/boss states override rest.

### 2.3 Authoring: music regions in Forge (TLS-02)

Music is spatially authored in the Forge zone editor plugin (TLS-02) using its volume system — Forge volumes are Godot editor plugin volumes, but the **authoring contract is unchanged** from v0.1:

- **Music Region volume**: references a `mus.*` set ID, with priority (overlaps resolved highest-wins — e.g. a camp sub-region inside a zone-wide region), default state, silence-scheduling overrides, and day/night set variant (WLD-02, optional per region).
- Region data is written to the zone's content-source files (TD-06) and compiled by TLS-01 into client paks; the client audio runtime evaluates which region the player occupies.
- **State inputs** are gameplay-driven, not volume-driven: server combat flag (CMB-01), threat/aggro-proximity events (CMB-02, client-observed), instance/boss-encounter scripting (GRP-02). The region says *what plays*; game state says *which layers*.

### 2.4 `ZoneMusicPlayer` runtime architecture

One reusable **`ZoneMusicPlayer`** runtime (built once at M0, parameterized forever after) replaces the former MetaSounds graph. Structure:

- Each state's stem stack is an **`AudioStreamSynchronized`** — all stems of the set play sample-locked in parallel, with per-stem volume used for the vertical mix (inaudible layers held at silence, never stopped, so L1 continuity is free).
- States/sections are clips in an **`AudioStreamInteractive`** wrapping those synchronized stacks; transition rules between clips carry the §2.2 quantization (next-bar / next-beat), fade curves, and fade lengths.
- Inputs: set ID (resolves stems + BPM/meter metadata from compiled content), target-state, per-layer gain targets. A stinger one-shot player (key-tagged pool per set) rides alongside on the music bus.
- A thin **MusicStateComponent** (GDScript/C++ node owned by the Client track — integration point below) decides state and drives the player; **all gameplay logic stays outside the audio graph** — the same principle as v0.1, and what makes the fallback (below) a playback-layer swap, not a re-author.
- Second reusable runtime **`AmbienceBed`** for §4 (crossfading multi-slice ambience, day/night interpolation), built from plain synchronized/looping streams — it needs no interactive transitions.

**What Godot's system does NOT give us (candidly), vs. MetaSounds:** there is no sample-accurate, node-based DSP graph we can script — no custom envelopes, clocks, or per-sample logic inside the stream. Transition options are **clip-level**: we get what `AudioStreamInteractive`'s transition table offers (timing mode, fade type, fade beats), not arbitrary quantized envelope shapes. Per-layer dynamic mixing inside a synchronized stack is coarse-grained (per-stem volume from script, not in-graph automation). Therefore the **M0 pipeline proof must validate**: bar-accurate transition timing (≤ 1 bar, no drift over a full 64–128-bar loop pass) held under load (50-client stress scene, streaming activity, min-spec CPU), stem lockstep across state changes, and equal-power fade quality at our tempi.

**Fallback (TD-11 decision gate, end of M0):** if the above fails, we build a **custom C++ GDExtension stem mixer** — our own bar clock + per-stem sample-accurate gain ramps feeding an `AudioStreamGenerator`/custom `AudioStream`. This is *not* commercial middleware: **FMOD and Wwise are explicitly off the table** — their licenses are incompatible with a fully-open, redistributable stack (TD-09 spirit; community members must be able to build and modify everything). This actually simplifies the old middleware decision gate: the fallback is code we own, ships under Apache-2.0, and consumes the identical stem assets and region data.

### 2.5 Integration point with the Client track

The Client PRD's audio runtime section owns: the MusicStateComponent, music-region evaluation at runtime, combat/instance state plumbing from the network layer, and the master bus layout. **This track owns**: the `ZoneMusicPlayer`/`AmbienceBed` runtimes, all audio assets, mix targets, and the authored region data. Contract: Client exposes a state API (`set_music_state(set_id, state)`, `play_stinger(stinger_id)`); Music guarantees any set conforming to the stem metadata schema plays correctly in `ZoneMusicPlayer`. The client PRD's audio integration section now exists and is being revised in parallel to expose the `AudioStreamInteractive` hooks (sync decision D-14, superseded by TD-11 v0.3 — see §10).

---

## 3. SFX framework (AUD-01)

### 3.1 Categories

| Category | Baseline features | Examples | First milestone |
|----------|------------------|----------|-----------------|
| Combat — melee | CMB-01 | swing whoosh, hit impact (3 weights), parry/dodge, miss | M1 |
| Combat — casting | CMB-01, CMB-04 | cast-loop per school (4 schools at M1), cast-complete, projectile travel, impact | M1 |
| Buffs/debuffs/auras | CMB-04 | apply/expire tones per school, aura loops (quiet, ≤2 concurrent audible) | M1 basic / M2 |
| Death & resurrect | CMB-03 | player death sting, corpse-run ghost ambience layer, resurrection shimmer | M1 |
| Foley — movement | CHR-02 | footsteps per surface (M1: dirt, stone, grass, wood, water shallow; M2: +snow, metal, mud), jump/land, swim strokes, armor-weight rustle (cloth/leather/plate) | M1 |
| UI | UI-01 | click, hover, open/close bags, error "dull thud", map open, quest-tracker updates | M1 |
| NPC vocalizations | NPC-01, NPC-02 | mob aggro bark, pain, death per family (M1: 6 mob families); vendor/trainer greet grunts (non-verbal at M1 — no VO in scope through M3) | M1 |
| Loot & rewards | ITM-02, QST-01 | loot-window open, item-pickup by class (coin, cloth, metal, potion), quest accept/complete fanfare | M1 |
| World interactables | WLD-03, ECO-02 | POI discovery (shared with music stinger), gathering loops (mining, herbing) | M1 / M2 |

### 3.2 Implementation

All SFX play with **round-robin (3–5 variations per event) + ±2 semitone pitch and ±1.5 dB volume randomization** — no machine-gunning. Variation/randomization uses `AudioStreamRandomizer` (random pitch/volume built in) wrapped by the SFX runtime. SFX hooks are **data-driven** (§7): a spell definition in `/content` references `sfx.*` IDs for cast/travel/impact; footstep surface types come from physics-material mappings authored once per material.

### 3.3 Attenuation & occlusion

- Spatial SFX play via **`AudioStreamPlayer3D`** with shared attenuation presets (small/medium/large/global) using natural-sound falloff (inverse-distance with max-distance clamps); combat SFX audible 5–40 m depending on class; UI is 2D (`AudioStreamPlayer`).
- **Occlusion:** a **single raycast** per audible spatial voice (listener → emitter, throttled round-robin) drives a low-pass + −6 dB duck when occluded, applied per-player via attenuation filter or an occluded send bus — the same single-trace budget as v0.1, and it **stays on min-spec** (TD-03). **No convolution reverb on min-spec**; reverb via lightweight **audio bus effects** (`AudioEffectReverb` on zone/interior buses) driven by Forge-placed reverb volumes (TLS-02). Baked/geometric acoustics is explicitly out of scope through M3.

### 3.4 Concurrency & voice limits (50+ player scenes, IT-M1)

Budget on min-spec (TD-03, GTX 1060-class / 16 GB):

- **Global cap: 64 real voices**, soft target ≤48 at the 95th percentile. Godot has no built-in per-group concurrency system, so this is owned code: **concurrency groups are implemented as audio buses + a voice-manager layer** (GDScript at M0/M1, promoted to a GDExtension if profiling demands) that gates play requests per group before a player node is ever created.
- Per-category concurrency groups: melee impacts 8, cast/impacts 10, footsteps 6 (nearest-N by distance), NPC vocal 6, ambient emitters 10, UI 4, music stems 8, stingers 2.
- Resolution rules: steal-quietest within group; **player-own actions always win** (priority boost); distance-based volume-weighted priority otherwise.
- Beyond-cap events **virtualize** (the voice manager tracks playback position on muted/stopped voices and resumes loops when a slot frees) rather than hard-cull, so loops recover.
- Validated in the IT-M1 50-player realm test (§8).

---

## 4. Ambient beds & emitters (AUD-03)

- **Zone ambience beds:** per zone, 2–3 looping bed layers (e.g. wind base + biome texture + fauna), each 60–120 s seamless loops, played via the `AmbienceBed` runtime (§2.4) and authored as **Ambience Volumes** in Forge (TLS-02) — Godot editor plugin volumes, same authoring contract as v0.1: priority, crossfade on boundary, 3–5 s.
- **Point emitters:** looping/one-shot spatial sources (waterfall, torch fire, tavern murmur, insect swarms) placed in Forge as emitter nodes referencing `amb.*` asset IDs; support randomized-interval one-shots (bird calls, creaks) with min/max retrigger times.
- **Day/night variation (WLD-02):** each bed defines day and night slices; `AmbienceBed` crossfades over the dawn/dusk window driven by the world time-of-day parameter (server-authoritative time). Night: fauna swap (birds→insects/owls), wind +2 dB relative, music regions may specify a night set variant (§2.3). Weather layers (rain/wind gusts) stack as an additional bed slice keyed to the WLD-02 weather state.
- Beds sit at −38 to −30 LUFS in-mix — present but under music and SFX.

---

## 5. Sourcing & provenance

Per TD-09 (unchanged for audio in v0.3): original music CC-BY 4.0; third-party CC0/CC-BY only (engine-locked marketplace audio was never permitted and remains disallowed); AI-generated allowed **with provenance recorded per asset**.

### 5.1 AI music generation workflow

1. **Generate** with tools whose terms grant redistribution-safe output usable in CC-BY-distributed projects (paid-tier commercial-output plans where required); the specific approved-tool list lives in `audio-direction.md` and is reviewed quarterly (legal landscape shifts — see §10).
2. **Never ship raw output.** Every AI-generated cue gets a human **editing/mastering pass**: re-cut to loop cleanly on the grid, stem separation or regeneration into the §2.1 layer scheme, key/tempo conform to the zone set, re-orchestration of weak sections, loudness mastering (§5.4). Target: ≥30% human transformation, logged in the provenance record.
3. **Quality bar:** an AI-assisted cue must pass the same blind in-engine review (§8.1) as composed cues; motif statements (main theme, race motifs) are **human-composed only** so the identity layer is unambiguously original. AI is for non-motif cues only.

### 5.2 CC0/CC-BY library vetting

- Approved sources: Freesound (CC0/CC-BY only — **no CC-BY-NC**, incompatible with our distribution), OpenGameArt, Sonniss GDC bundles (per their license), Kevin MacLeod-style CC-BY music (attribution honored per 5.3), public-domain recordings.
- Vetting checklist per asset: license verified at source URL, no NC/ND/SA clause (SA is incompatible with per-asset CC-BY-4.0 policy), uploader plausibly the rights holder (reverse-search suspicious uploads), attribution string captured.

### 5.3 License tracking per asset

Every audio asset has a **provenance record** in the content database, adjacent to its asset-ID entry (baseline §5.3 asset ID registry): `source` (original / ai / library), `tool_or_library`, `source_url`, `license`, `attribution`, `author`, `transform_notes`, `date`. TLS-07 content CI **fails the build** on any audio asset missing a provenance record. A generated `CREDITS-AUDIO.md` aggregates all CC-BY attributions for shipping.

### 5.4 Loudness & mastering standards

| Content | Integrated loudness | True peak |
|---------|--------------------:|----------:|
| Music stems (per-state full mix) | −16 LUFS | −1 dBTP |
| Ambient beds | −28 LUFS (mixed to −38..−30 in-game) | −3 dBTP |
| SFX one-shots | −16 LUFS-S (short-term), normalized per category | −1 dBTP |
| UI sounds | −20 LUFS-S | −3 dBTP |
| Whole-game target at default sliders | −18 LUFS integrated during normal play | −1 dBTP |

Measured with an ITU-R BS.1770-4 meter; CI-checkable via an offline analysis script in the audio pipeline.

### 5.5 File formats & compression budgets

- **Source of truth:** 48 kHz / 24-bit WAV in Git LFS (TD-12), stems + session files where available — unchanged.
- **In-engine:** **Ogg Vorbis**, Godot's native streaming format. Music & beds at quality ~0.7 (≈192 kbps stereo, transparent for our material), SFX at ~0.8; music and beds stream from disk (`AudioStreamOggVorbis`); SFX ≤3 s may be WAV-in-pak (PCM) for zero-decode latency where profiling favors it; stingers preloaded per active set. Loop points are baked at import so seams survive compression.
- **Budgets (min-spec, TD-03):** audio memory **≤ 150 MB resident per zone** (streamed music excluded) — unchanged; compressed pak size ≤ 60 MB per zone music set, ≤ 25 MB zone ambience, ≤ 80 MB global SFX bank at M1 growing to ≤ 150 MB by M3. Vorbis lands within ~10% of the old codec's sizes at these quality settings; budgets hold without renegotiation.

---

## 6. Asset scope by milestone

Counts are planning targets (±20%), not contractual.

### M0 — Foundation (pipeline proof)
- Audio direction doc (§1.3).
- **1 adaptive zone track**: 5 stems (L1×1, L2×1, L3×1, L4×2), 2 stingers (combat entry/exit), running in `ZoneMusicPlayer` on the empty test map with debug state switching.
- `ZoneMusicPlayer` v1 runtime (`AudioStreamInteractive` + `AudioStreamSynchronized`); the TD-11 decision-gate evidence (bar-accuracy-under-load measurements, §2.4) delivered to cross-track review; asset-ID + provenance conventions registered with Tools track for TLS-01.
- ~10 placeholder SFX (footstep, UI click, login sting) to prove the SFX ID → content-file hook path.

### M1 — Greybox Vertical Slice
- **Zone-01 basic music set:** 1 full adaptive set (6 stems) + 5 stingers (combat in/out, discovery, level-up, death) + main theme v1 on login (CHR-01 is M0 stub / M1).
- **Core SFX: ~220 sounds** — melee (~30), casting for 2 classes / 4 schools (~40), footsteps 5 surfaces × 4 variations × walk/run (~40), death/rez (~8), 6 mob families × aggro/pain/death × variations (~54), UI (~20), loot/quest/vendor (~25).
- Voice-limit/concurrency framework (buses + voice manager, §3.4) in and validated at 50 players (IT-M1).
- 1 provisional ambience bed for Zone-01 (AUD-03 is M2, but IT-M1 shouldn't be silent between music cues) — flagged provisional, replaced at M2.

### M2 — Systems Depth
- **Zone-01 full adaptive score** (matches "beautiful corner" art pass): expanded to 7 stems + night-variant explore layer (WLD-02), full stinger set (~10).
- **Dungeon-01 music system (GRP-02):** dungeon set (6 stems) + boss set (5 stems + boss entry/kill stingers); instance scripting drives boss state.
- **Ambient beds & emitters (AUD-03):** Zone-01 final beds (3 layers × day/night) + ~30 placed point-emitter assets; Dungeon-01 interior beds (2); weather layers (rain, wind) for WLD-02.
- **SFX: ~150 new** — talents/new abilities (CHR-04, ~30), buffs/auras (CMB-04 full, ~25), crafting/gathering loops (ECO-02, ~20), auction/mail UI (ECO-03/04, ~10), group/dungeon (GRP-01/02, ~15), Zone-02-greybox creature families (~50).
- Music-region + ambience-volume authoring shipped in Forge (TLS-02) and used for all of the above (Pillar 1).

### M3 — Alpha World
- **Zones 02–04 adaptive sets:** 3 zone sets (6–7 stems each) + stingers, day/night variants; beds + emitters for 3 zones (~25 emitter assets each); Dungeon-02 set.
- **Battleground (PVP-02):** BG set with explore(staging)/tension(objective contested)/combat layers + score/flag stingers (~6).
- **Character-select/login theme (CHR-01):** final main theme, full arrangement (~3 min) + 2 race motif themes on race-select.
- **SFX: ~210 new** — 2 new classes (~60), race-specific foley/vocal variants (~20), new creature families ×~8 (~70), PvP/BG (~15), guild/social/LFG UI (SOC-02/GRP-03, ~10), zone interactables (~25), ground mounts (CHR-05: mount/dismount, gallop foley per surface, mount vocal set, ~12).
- Cumulative at M3: ~5 zone-scale adaptive sets + 3 dungeon/BG sets, ~570 SFX, ~12 beds, ~110 emitter assets.

### M4 — direction only (per baseline)
Localization-safe audio (no baked-in language), accessibility (visual-cue parity for critical audio, sliders per category), first-raid score. Scope set at end of M2.

---

## 7. Integration with tools

- **Forge zone editor (TLS-02, Godot editor plugin):** this track specifies (and Tools implements) three authorable audio primitives — **Music Region volumes** (§2.3), **Ambience Volumes** (§4), **Point Emitter nodes** (§4) — all serialized into the zone's content-source files (TD-06), all referencing asset IDs only. Because Forge is a Godot editor plugin, audio authors preview regions in the same editor the runtime uses — no export round-trip.
- **Asset IDs (baseline §5.3):** naming scheme registered in the ID registry, e.g. `mus.zone01.explore.layer2`, `mus.zone01.stinger.combat_enter`, `mus.dun01.boss.layer1`, `sfx.cmb.melee.impact.heavy`, `sfx.ui.error`, `sfx.foley.footstep.stone`, `amb.zone01.bed.night`, `amb.emitter.waterfall.large`. IDs are permanent; file paths are never referenced from content.
- **Content database & compiler (TLS-01):** audio metadata (BPM, meter, loop length, stem membership, LUFS, provenance §5.3) lives in `/content` YAML next to the ID; the compiler emits the client-side ID→pak-asset lookup and the stream metadata (beat count, bar length) `ZoneMusicPlayer` consumes. Provenance completeness and loudness bounds are TLS-07 lint rules.
- **Data-driven SFX hooks (unchanged):** spell/ability definitions (authored in the NPC/item/quest editors, TLS-03/04/05) carry `sfx_cast`, `sfx_travel`, `sfx_impact` ID fields; NPC definitions carry vocal-set IDs; item classes carry pickup/equip IDs; quest definitions carry accept/complete IDs. Music/audio never requires a code change to wire a sound to content.

---

## 8. Testing & acceptance

### 8.1 In-engine review process
- Weekly **audio review build**: fly-through of current zones with debug HUD showing active music region, state, bar position, voice count. All new assets reviewed in-engine (in Godot, not in a DAW) before merge.
- Blind A/B checks for §1.2 criteria at each milestone gate; loop-seam and loudness checks automated in the pipeline (§5.4), including a post-Vorbis-encode seam check.

### 8.2 Performance validation on min-spec (TD-03)
- Voice count ≤64 (soft ≤48), audio-thread + voice-manager CPU ≤ 5% of frame budget, audio memory within §5.5 budgets — measured **in-Godot** (engine profiler + custom voice-manager telemetry) on the GTX 1060/16 GB reference machine in the standard 50-player stress scene each milestone.
- M0 adds the TD-11 gate measurement: transition-timing error vs. the musical grid, logged across repeated state flips under stress (§2.4).

### 8.3 Contribution + done-criteria per integration test

| IT | This track contributes | Done means |
|----|------------------------|------------|
| **IT-M0** | 1 adaptive track in `ZoneMusicPlayer` on the test map; login UI click + footstep placeholders wired via asset IDs through TLS-01 | Two connected clients both hear the track; debug state switch produces a bar-quantized layer change with no seam; transition timing measured bar-accurate under load (TD-11 gate evidence); asset IDs resolve from compiled content, not hardcoded paths |
| **IT-M1** | Zone-01 music set with real combat-state switching; ~220 core SFX covering the level-1→5 quest chain (combat, footsteps, UI, loot, quest, death/rez) | The 10-quest chain plays fully sounded; combat entry/exit transitions correct against server combat flag; 50+ player town scene holds voice/CPU budgets (§8.2) with player-own actions always audible |
| **IT-M2** | Dungeon-01 + boss music driven by instance scripting; Zone-01 final score + beds/emitters authored via Forge; crafting/AH/mail SFX for the economy loop | 5-player Dungeon-01 clear: boss music enters/exits on encounter script; all Zone-01 audio regions authored in TLS-02 (zero hand-placed engine nodes); economy-loop actions each have audible feedback |
| **IT-M3** | Zones 02–04 + BG scores; login theme; audio primitives documented for community creators (TLS-08) | 500-CCU alpha holds audio perf budgets; BG music states track objective/combat state in a 10v10; a community-made zone with its own music regions/ambience volumes plays correctly on unmodified client+server |

---

## 9. Traceability table

Every feature where Music is ● in the baseline matrix (§4):

| Feature ID | Audio deliverable(s) | Milestone |
|------------|----------------------|-----------|
| CHR-01 | Character-select/login theme (main theme v1 at M1, full arrangement + race motifs at M3); create-screen UI SFX | M0 stub / M1, final M3 |
| CHR-05 | Ground-mount audio under the AUD-01 framework: mount/dismount SFX, mounted-locomotion foley per surface, mount vocal sets | M3 |
| WLD-01 | Music-region + ambience streaming behavior across zone streaming boundaries (regions load/crossfade with chunk-streaming cells, WLD-01 chunk format IF-6) | M1 |
| WLD-02 | Day/night bed slices + night music variants; weather ambience layers (rain/wind) | M2 |
| WLD-03 | Discovery stinger + POI-scoped music sub-regions; map-open UI sound | M1 |
| CMB-01 | Combat layer state switching (AUD-02); melee/cast/impact SFX per school | M1 |
| CMB-03 | Death sting, ghost/corpse-run ambience layer, resurrection SFX | M1 |
| QST-01 | Quest accept/complete fanfares; tracker-update UI SFX | M1 |
| ITM-02 | Loot-window and per-item-class pickup SFX | M1 |
| GRP-02 | Dungeon-01 music set + boss state system + instance-scripting hook | M2 |
| PVP-02 | Battleground adaptive set + objective/score stingers | M3 |
| AUD-01 | SFX framework: categories, concurrency groups (buses + voice manager), attenuation/occlusion presets, data-driven hooks; includes buff/debuff SFX (confirmed under AUD-01, decision D-13) | M1 |
| AUD-02 | `ZoneMusicPlayer` (AudioStreamInteractive + AudioStreamSynchronized), stem architecture, transition rules, music-region authoring; basic M1 → full (dungeon/night/silence-scheduling) M2 | M1 basic / M2 |
| AUD-03 | `AmbienceBed`, zone beds, point emitters, Forge ambience volumes; Art delivers the visual emitter counterparts (torch flames, waterfalls — Art's ● on AUD-03 confirmed per decision D-12) | M2 |
| TLS-02 | Spec + acceptance for Music Region volumes, Ambience Volumes, Point Emitters in Forge (Tools implements; Music defines & validates) | M1 |

---

## 10. Risks & open questions

### Risks

1. **AI-music licensing ambiguity (high).** Terms of AI music tools change frequently and copyrightability of AI output is unsettled; CC-BY 4.0 requires rights we may not cleanly hold over raw AI output. Mitigation: §5.1 human-transformation requirement, per-asset provenance (TD-09), quarterly approved-tool review, motif/theme layer human-composed only, and the ability to regenerate/replace any flagged cue since content references IDs not files.
2. **`AudioStreamInteractive` expressiveness (medium).** Godot's interactive audio is clip-level, not a scriptable DSP graph (§2.4): transition behavior is limited to its built-in table, per-stem mixing is script-driven, and bar-accuracy under real load is unproven for our stem counts. Mitigation: this is exactly what the M0 pipeline proof validates (TD-11 decision gate, end of M0, with measured evidence per §8.2); all gameplay logic stays outside the audio layer, so the fallback — **our own GDExtension stem mixer**, not commercial middleware (FMOD/Wwise license-incompatible with a fully-open project) — swaps the playback layer only, and the authoring data, stems, and region format survive unchanged. Compared with v0.1, the gate is simpler: both branches are fully open source, so it is an engineering decision, not a licensing one.
3. **Contributor sourcing (medium).** Open-source projects rarely attract composers early; ~5 adaptive sets + ~570 SFX by M3 is a real workload. Mitigation: AI+library pipeline keeps a single audio lead viable through M1; publish the audio direction doc + stem templates as contributor onboarding; small, well-scoped "one mob family vocal set" style issues. The fully-open Godot stack lowers the contribution barrier — no engine license or launcher required to open the audio project.
4. **50-player mix legibility (medium).** Even under voice limits, dense scenes can smear. Mitigation: priority rules in §3.4, early stress testing at IT-M1 rather than IT-M3. The voice manager is owned code (not engine-provided as in UE), so its behavior under stress is itself a test target from M1.

### Resolved since v0.1

1. **Client PRD audio integration (was open question 1; sync decision D-14, superseded by TD-11 v0.3):** the client PRD's audio integration section now exists and is being revised in parallel to expose the `AudioStreamInteractive` hooks; the §2.5 contract is now reconciled against it rather than proposed blind.
2. **CMB-04 buff/debuff SFX (was open question 2; decision D-13):** confirmed delivered under AUD-01 — no baseline matrix change needed.
3. **Art's role on AUD-03 (decision D-12):** Art's ● on AUD-03 confirmed as the visual emitter counterparts (particle/mesh sources our point emitters attach to); reflected in §9.
4. **Middleware question (was implicit in v0.1 risk 2):** FMOD/Wwise ruled out on licensing grounds by the v0.3 open-stack pivot; the TD-11 fallback is a first-party GDExtension mixer.

### Open questions (no new IDs invented; listed here per instructions)

1. **No VO feature ID:** NPC-01/NPC-02 dialogue is text-only through M3 per this PRD (non-verbal vocalizations only). If voiced dialogue is ever wanted, it needs a baseline feature ID and localization implications (M4 direction).
2. **Music player-settings ownership:** category volume sliders live in UI-01/options — which track owns the audio-options UI spec (Client or Music) is unassigned.
3. **Weather taxonomy (WLD-02)** is undefined in the baseline (which weather states exist?); ambience weather layers in §4 assume at least rain + wind and need the Server/Client weather-state enum.
4. **Community audio (TLS-08):** can community zones ship their *own* audio files, or only reference shipped asset IDs? Provenance enforcement (TLS-07) for community-supplied audio is unresolved — sharper now that community members build with the same open editor we do.
5. **GDExtension fallback scoping:** if the M0 gate triggers the fallback, the custom mixer needs an owner (Client or Music track engineering) and a time-box; unassigned until the gate outcome is known.
