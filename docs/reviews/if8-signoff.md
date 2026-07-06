# IF-8 sign-off prep — `asset.schema.yaml` (issue #72, A-12 close-out)

**Status:** Prep for owner sign-off (#72) — surfaces what to verify; sign-off recorded in sync log by the owner.

**Scope:** the last open step of **A-12** (Sync Decisions §7 / D-18): Art + Music sign-off of the `meridian/asset@1` schema PR. The schema is already authored, validated, and exercised by 19 example sidecars (Sync Decisions §7 "A-12 — DISCHARGED (authoring)"; Tools SAD §4). This doc does **not** decide sign-off — it lays the D-18 field blocks side-by-side against the real authoring need each serves so the owner (Art+Music owner) can ratify quickly and see exactly what, if anything, is flagged.

**Read against:**
- `schema/content/asset.schema.yaml` — the D-18 union (core envelope + `art.*` sidecar fields + `music`/`sfx`/`amb`-serving audio blocks).
- `docs/prd/art-prd.md` — authoring requirements (§2 budgets, §3 provenance, §4.2 import, §6 kit contract).
- `docs/prd/music-prd.md` — authoring requirements (§2 adaptive music, §3 SFX, §5.4 loudness).
- `docs/01-SYNC-DECISIONS.md` — D-18 (field union), A-12 (authoring task), §7.1 / D-31 (pack-local ruling: source + sidecars at `content/<ns>/assets/**`, `source:` pack-root-relative).

Field-block anchor: schema lines are cited so the owner can jump straight to each block.

---

## Track A — Art

Fields the Art track owns in D-18: `import_hints`, `contract_envelope`, `restyle_status`, `reviewed_by`, `tags`, plus the shared core envelope (`class`, `source`, `license`, `provenance`) as it serves art authoring.

| Schema field / block | What the authoring workflow needs it for | Present? | Gap or over-constraint? |
|---|---|---|---|
| `class` enum (art values: `character_model`…`ui_art`) — L14–20 | Drives import preset + budget-row selection (PRD §2.1 geometry, §2.3 texture, mapping to "Art SAD §2.3 preset table") | ✅ | None. Covers every PRD §2.1/§5 asset family (character/armor/weapon/mob/boss/critter/prop/kit/landmark/foliage/texture/icon/vfx/ui). |
| `source` (`^assets/...`, pack-root-relative) — L21–24 | The hand-authored `.glb`/`.png`, pack-local per D-31/§7.1 (PRD §3.3, §4.3) | ✅ | None. Pattern enforces the pack-local `assets/` prefix; matches the D-31 ruling exactly. `/client/art` (imported output) is correctly **not** a `source` location. |
| `extra_sources` — L25–29 | Multi-file assets: texture sets (PRD §2.3 "trim sheets + tiling materials"), multi-part kits | ✅ | None. |
| `provenance{source_tier, origin_url, authors, attribution, license_verified_on, ai{}, transform_notes}` — L34–67 | TD-09 provenance-per-asset; the whole §3.3 record lives here (PRD §3.1 tiers, §3.2 AI workflow, §3.4 restyle) | ✅ | None. `ai{tool, prompts_file}` matches PRD §3.2 prompt-hygiene ("prompts stored in the provenance record, auditable"). `allOf` conditionals enforce ai/cc0/cc_by extra requirements — matches §3.3. |
| `license` enum `[CC0-1.0, CC-BY-4.0]` — L30–32 | SPDX allowlist; §3 "engine-locked disallowed", §3.5 "CC-BY-SA not accepted" | ✅ | None. Enum hard-fails anything outside CC0/CC-BY (PRD §3.3 "fails CI outright"). SA/NC/ND cannot be expressed → correctly impossible. |
| `import_hints{lod_policy, lightmap_uv2, occluder, multimesh_safe}` — L70–77 | Import-validator inputs (PRD §4.2): LOD behavior, lightmap UV2 for statics, occluder gen for wall-class, MultiMesh-safety (§2.5, §6.1 kit contract) | ✅ | None on the four fields. **See Flag A1** — these are import *hints*, not the budget itself. |
| `contract_envelope{pivot, aabb_min, aabb_max, collision_hash}` — L78–86 | Greybox→art-pass 1:1 swap contract (PRD §6.2: "same pivot, bounds, collision envelope"); snapshot at greybox merge | ✅ | None. Directly encodes the §6.2 "1:1 by asset ID" deviation guard. |
| `restyle_status {not_applicable, pending, done}` — L87–89 | Tier B/C restyle gate (PRD §3.4, §8.2 item 7: "tier B/C restyle checklist done"); "ai/cc0/cc_by cannot merge as pending" | ✅ | None. Note: schema comment states the merge rule; **enforcement of "cannot merge as pending" is a lint (L02x), not schema** — confirm the lint exists (see Flag A2). |
| `reviewed_by[]` — L90–93 | Two-reviewer style-gate sign-offs (PRD §7.2, §4.4 style gate); "written by the review bot, not self-attested" | ✅ | None as a data field. Schema cannot enforce the two-reviewer *count* or bot-authorship — that is review-flow/lint policy (Flag A2). |
| `tags[]` (`^[a-z][a-z0-9_]*$`) — L94–97 | Forge palette/browser filter tags (PRD §6.1 "registered with category tags so TLS-02 palettes filter"; TLS-02) | ✅ | None. |

### Art — not-in-schema, by design (confirm this is intended, not a gap)

- **Geometry / texture / VRAM budgets** (PRD §2.1 tri counts, §2.3 texel density + VRAM) — **not schema fields**. PRD §2 states these are enforced "via the asset review flow (§4.4) and the min-spec perf gate (§8.1)" and "at authoring time … verified on the bench machine," i.e. bench/review-time, not sidecar metadata. Flagged A1 for explicit owner confirmation that budgets are deliberately *out* of the sidecar.

---

## Track B — Music / Audio

Fields the Music track owns in D-18: `loudness`, `music{}`, `sfx{}`, `encode{}`, plus the audio `class` values and the `amb.*`-serving classes.

| Schema field / block | What the authoring workflow needs it for | Present? | Gap or over-constraint? |
|---|---|---|---|
| `class` audio values (`music_stem`, `music_stinger`, `sfx`, `ui_sound`, `ambience_bed`, `ambience_emitter`) — L18–20 | Encode-tier + budget selection; feeds the required-block conditionals below (PRD §2.1 stems, §2.2 stingers, §3 SFX, §4 beds/emitters) | ✅ | None. `ambience_bed` + `ambience_emitter` cover AUD-03 (§4); the `amb.*` ID prefix (D-24) resolves to these classes. |
| `loudness{lufs_integrated (≤0), true_peak_dbtp (≤0)}` — L100–106 | Loudness/mastering standards, ITU-R BS.1770-4 (PRD §5.4); CI-checkable bounds (PRD §5.4 "CI-checkable via offline analysis script") | ✅ | **See Flag M1** — one integrated-LUFS field; PRD §5.4 uses **LUFS-S (short-term)** for SFX one-shots and UI. Schema field name is `lufs_integrated` only. |
| `music{stem_set, layer, bpm, time_signature, length_bars, key, loop}` — L107–122 | `ZoneMusicPlayer` stream metadata (PRD §2.4): BPM+meter mirrored into stream beat properties (§2.2), stem membership (§2.1), key-tagged stingers (§2.2) | ✅ | None. `layer` enum `[L1,L2,L3,L4,boss,stinger]` matches §2.1 layer table + §2.1 boss stem + §2.2 stingers. `length_bars`/`bpm`/`time_signature` feed the §2.2 bar-quantized transitions. |
| `music.key` (mode-tagged regex) — L118–121 | Stinger key-tag matching so stingers "land in key" (PRD §2.2, §2.4 key-tagged stinger pool; "Music SAD §2.3") | ✅ | **See Flag M2** — the regex allows one mode per root (`a_major`…`g_sharp_minor` + church modes). Confirm no set uses a key outside this vocabulary (e.g. atonal / no-key texture beds that would need to *omit* key — `key` is optional, so omission is allowed; verify). |
| `sfx{category, variation_group, attenuation}` — L123–136 | SFX runtime hooks: `category` → concurrency group + bus (PRD §3.4 groups, §3.3 buses); `variation_group` → round-robin siblings (§3.2); `attenuation` presets (§3.3 small/medium/large/global + ui2d) | ✅ | None. `attenuation` enum `[small, medium, large, global, ui2d]` matches §3.3 presets + the 2D UI case. `category` pattern permits the dotted namespacing (`cmb.melee.impact`) the PRD §7 IDs use. |
| `encode{tier {default, wav_passthrough}, preload}` — L137–142 | Format/streaming decisions (PRD §5.5): Ogg default vs WAV-in-pak for ≤3 s SFX; stinger preload per active set (§2.2, §5.5 "stingers preloaded per active set") | ✅ | None. `wav_passthrough` = the §5.5 "WAV-in-pak (PCM) for zero-decode latency"; `preload` = the stinger-preload rule. |
| `allOf` audio conditionals — L157–171 | `music_stem`/`music_stinger` ⇒ require `music`+`loudness`; `sfx`/`ui_sound`/`ambience_emitter` ⇒ require `sfx`; `ambience_bed` ⇒ require `loudness` | ✅ | None. Matches the "required for … classes" comments and PRD §2.1/§3/§4 authoring reality. **See Flag M3**: `ambience_bed` requires `loudness` but **not** `sfx` — beds are Forge-volume-placed (§4), not concurrency-grouped like emitters; confirm this asymmetry (bed vs emitter) is intended. |

### Music — not-in-schema, by design (confirm)

- **`ui_sound` loudness:** `ui_sound` is required to carry `sfx{}` but **not** `loudness{}` (only stems/stingers/beds require loudness). PRD §5.4 lists a UI-sound loudness target (−20 LUFS-S). Rolled into **Flag M1** — decide whether UI/SFX loudness belongs in the sidecar or stays a pipeline-analysis (§5.4 offline script) check.

---

## Flagged for decision

These are the only mismatches surfaced. None blocks sign-off on its own; each needs an owner ruling of **intended / amend**.

| # | Track | Flag | What to decide |
|---|---|---|---|
| **A1** | Art | Geometry/texture/**VRAM budgets** (PRD §2.1/§2.3) are **not** sidecar fields — enforced at review/bench (§4.4, §8.1). | Confirm budgets are deliberately out-of-schema (bench-enforced), not a missing field. Likely **intended** — the PRD says so explicitly. |
| **A2** | Art | `restyle_status` "cannot merge as pending" and `reviewed_by` two-reviewer/bot-authored rules are **policy the schema can't express** — they depend on lints (L020–L022) / the review flow, not the field shapes. | Confirm the corresponding lints exist and are wired (Sync Decisions §7 cites L020; Tools SAD §2.2/§4.3 lint bands). Schema itself is fine. |
| **M1** | Music | `loudness.lufs_integrated` is **integrated-only**; PRD §5.4 uses **LUFS-S (short-term)** for SFX one-shots + UI, and `ui_sound`/`sfx` classes don't require a `loudness` block at all. | Decide: (a) add an optional short-term field / allow `loudness` on `ui_sound`/`sfx`, or (b) ratify that per-category SFX/UI loudness stays a pipeline-analysis check (§5.4 offline script) and the sidecar carries integrated LUFS only for music/beds. Leaning (b) — but it is a real field-vs-pipeline choice for the owner. |
| **M2** | Music | `music.key` regex fixes one-mode-per-root vocabulary. | Confirm every planned set's key fits the vocabulary, or that key-less textures simply omit the (optional) field. Likely **intended** (key is optional + covers major/minor/church modes). |
| **M3** | Music | `ambience_bed` requires `loudness` but not `sfx{}`; `ambience_emitter` requires `sfx{}`. | Confirm the bed-vs-emitter asymmetry is intended (beds are Forge-volume-placed and loudness-bounded, §4; emitters are concurrency-grouped spatial voices, §3.4). Reads as **intended**. |

**Net:** 0 hard gaps found. All five flags read as "intended, confirm" except **M1**, which is a genuine field-vs-pipeline design choice worth an explicit owner ruling.

---

## Sign-off checklist (owner ticks)

**Art track owner:**
- [ ] `class` art-value enum covers every asset family I author (PRD §2.1/§5). *(prep says: yes)*
- [ ] `import_hints` cover every import-validator input I rely on (PRD §4.2). *(yes)*
- [ ] `contract_envelope` is sufficient for the §6.2 greybox 1:1 swap guard. *(yes)*
- [ ] `provenance` block (incl. `ai{}`) captures the full §3.3 record + prompt hygiene (§3.2). *(yes)*
- [ ] **Flag A1** ruled: budgets stay bench-enforced, deliberately out of the sidecar.
- [ ] **Flag A2** ruled: restyle/review-count enforcement is lint/flow, schema shape accepted.

**Music track owner:**
- [ ] `music{}` block feeds `ZoneMusicPlayer` stream metadata completely (PRD §2.4). *(yes)*
- [ ] `sfx{}` (`category`/`variation_group`/`attenuation`) covers the §3.3/§3.4 runtime. *(yes)*
- [ ] `encode{}` covers the §5.5 Ogg/WAV/preload decisions. *(yes)*
- [ ] Audio `allOf` required-block conditionals match my per-class authoring reality (§2.1/§3/§4). *(yes)*
- [ ] **Flag M1** ruled: per-category SFX/UI loudness stays pipeline-checked **or** schema amended.
- [ ] **Flag M2 / M3** ruled: key vocabulary + bed/emitter asymmetry confirmed intended.

**Both:**
- [ ] Pack-local location (`content/<ns>/assets/**`, `source:` pack-root-relative, D-31/§7.1) is correct.
- [ ] Sign-off recorded in the sync log (this doc is prep only; the owner records the decision).
