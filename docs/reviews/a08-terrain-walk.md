# A-08 requirements walk prep — chunk format v1 (issue #70, IF-6)

**Status:** Pre-walk for A-08 (#70) — accept/amend proposals; final format ratified by the owner.

**Scope:** A-08 is the sign-off walk for the **IF-6 zone chunk format v1** (Tools SAD §3, `DRAFT`). Per **D-20** the walk is a line-by-line check of the **Client's nine-point requirements list** (Client SAD §5.3) + the **Server's cell-metadata needs** (Server SAD §4.5) against the **Tools SAD §3** draft format. This doc pre-fills that walk with an **accept / amend** proposal per line so the owner just ratifies (or overrides). It does **not** ratify the format.

**Read against:**
- `docs/sad/client-sad.md` §5.3 — the nine-point terrain/chunk-metadata requirements list (input to A-08).
- `docs/sad/server-sad.md` §4.5 — server cell-metadata consumption (walkable, liquid, navmesh tile refs, via IF-4).
- `docs/sad/tools-sad.md` §3 — chunk format v1 draft (§3.1 coords, §3.2 grid/heightfield, §3.3 manifest, §3.4 per-chunk payload); §5.2 `ITerrainBackend.export_heightfield → f32[129×129]` (adjacent — the heightfield-extraction seam).
- `docs/01-SYNC-DECISIONS.md` §11 — A-09 terrain decision (fork-and-vendor Terrain3D; 128 m region alignment PASS; native f32 heightfield PASS).

---

## Client nine-point list (Client SAD §5.3) vs Tools SAD §3

| # | Need (client point) | Covered by Tools SAD §3? | Accept / Amend | Note |
|---|---|---|---|---|
| C1 | **Grid definition in zone manifest** — cell size (m), origin, world→cell transform, zone bounds; one uniform grid per zone | ✅ §3.3 manifest: `chunk_size_m`, `origin{x,z}`, `grid{min/max cx/cz}`; §3.1 zone-local coords settle the transform | **Accept** | 128 m uniform cell (§3.2). World→cell transform is implicit (zone-local origin + `chunk_size_m`); explicit but derivable. |
| C2 | **Per-chunk scene reference** — one Godot scene per cell, by asset ID→`res://`, never a raw path | ⚠️ §3.4 client payload names a concrete `res://…/<cx>_<cz>.scn` path; §3.3 `chunks[].scene` field | **Amend (minor)** | Client requires the manifest ref be an **asset ID resolved to `res://`, never a raw path** (§5.3 pt 2). §3.3 `"scene": "…"` is unspecified as ID-vs-path. **Propose:** manifest `scene`/`proxy`/`server` fields are documented as asset-ID (or ID-derived) refs, not raw `res://` strings, to hold the IF-8 "no raw path in manifest" rule. |
| C3 | **Proxy/imposter reference per chunk** — baked low-poly proxy (or explicit `none`), independently loadable, fits Low draw-call budget | ✅ §3.2 LOD rings (ring ≥2 = proxy mesh); §3.4 `…<cx>_<cz>.proxy.scn`; §3.3 `chunks[].proxy` | **Accept** | §3.2 "format only guarantees a proxy exists per chunk" matches the requirement. **Confirm** the explicit-`none` case: sparse chunk list allows holes, but an existing chunk with no proxy needs a representable `none` (propose documenting `proxy: null` as the explicit-none form). |
| C4 | **Load priority & dependency metadata** — per-chunk priority hint + shared-dependency list for pre-warming | ⚠️ §3.3 `chunks[].deps` (asset-ID prefetch list) present; **priority hint absent** | **Amend** | `deps` covers the shared-dependency half. The **per-chunk priority hint** (ground/collision vs decoration split, or ordinal) from §5.3 pt 4 is **not** in the §3.3 manifest. **Propose:** add optional `priority` (ordinal or enum) to the manifest `chunks[]` entry. |
| C5 | **Per-chunk AABB + content hash** — AABB for visibility/priority scoring; hash for pack verify + patch granularity | ⚠️ §3.3 `chunks[].hash` present (blake3, covers both payloads); **AABB absent from manifest** | **Amend** | Hash: **accept** (§3.3 `hash`, §3.4 "hash covers both payloads"). AABB: the manifest has no per-chunk AABB — the client needs it for visibility/priority scoring **before** loading the scene. §3.2 gives a heightfield (server payload) but not a cheap manifest-level AABB. **Propose:** add `aabb{min,max}` to the manifest `chunks[]` entry (derivable at export from terrain bounds + placements). |
| C6 | **Ambient audio placements (AUD-03)** ride the chunk data — emitter/bed volumes instantiate on stream-in | ✅ §3.4 client payload: "ambience/music volume nodes" in the compiled scene | **Accept** | Present in the client `.scn` payload, matching Client SAD line 118 (emitters ride IF-6 chunk data) and Music PRD §4/§7. No manifest-level entry needed — they live in the scene. |
| C7 | **Manifest format version field** + versioning/migration policy | ✅ §3.3 `format_version`; §3.4 server file leads with `format_version` (uint16); §3.4 migration policy (re-export, N−1 loader support) | **Accept** | Matches §5.3 pt 7 and the client's restated preference (Client SAD §5.5 item 4: mandatory version field, N/N−1 loader). Policy half is due M1 start per §3.4 — on schedule. |
| C8 | **Deterministic export** — identical Forge input ⇒ identical chunk bytes/hashes | ✅ §3.4 "hash covers both payloads for incremental invalidation"; Tools SAD §2.5 bit-identical Recast tiles; double-build honesty | **Accept** | Determinism is a stated Tools principle (Principle 3 / PRD R4). **Confirm** the FlatBuffers server payload (§3.4) is byte-deterministic (field ordering, no map iteration nondeterminism) — cheap to assert in the double-build test. |
| C9 | **Interest alignment guarantee** — server-AoI cell metadata uses the *same* grid definition, so "client radius ≥ server radius" is manifest-checkable | ✅ §3.4 server payload cell metadata: "AoI cell id (= chunk coord)"; §3.1 shared zone-local axes; Server SAD §4.5 consumes cell metadata on the same grid | **Accept** | The AoI cell id **is** the chunk coord — one grid, both consumers. Server SAD §4.5 + §4.7 confirm the server takes the server-facing slice on that grid. Meets §5.3 pt 9 directly. |

---

## Server cell-metadata needs (Server SAD §4.5) vs Tools SAD §3

Server consumes only the *server-facing slice*, delivered inside the IF-4 artifact set (mcc bakes it; `worldd` never parses Forge scenes).

| # | Server need | Covered by Tools SAD §3? | Accept / Amend | Note |
|---|---|---|---|---|
| S1 | **Per-cell walkable metadata** (movement validation, OPS-03) | ✅ §3.2 heightfield 129×129 @ 1 m f32; §3.4 server payload heightfield block + heightfield collider params | **Accept** | 1 m heightfield resolution "suffices for movement validation" (§3.2). Server SAD §4.5 "per-cell walkable metadata" = the heightfield + walkable-tagged terrain (§5.2 op 5). |
| S2 | **Liquid volumes** | ✅ §3.4 cell metadata: "liquid regions (type + surface height)" | **Accept** | Type + surface height matches Server SAD §4.5 "liquid volumes." |
| S3 | **Navmesh tile references** | ✅ §3.2/§3.4 navmesh: 4×4 Recast tiles/chunk (Detour serialized, versioned by vendored Recast rev); Tools SAD §2.5 identical Recast across `forge_core`/`mcc`/`worldd` | **Accept** | 32 m tiles, 4×4/chunk (§3.2). Bit-identical bake (§2.5) is the correctness anchor. **Note (registry, not format):** Server SAD §10(b) flags navmesh binary artifacts have no IF-4 registry coverage — that's a registry-ownership question, **out of scope for the format walk**; call it out so the owner routes it separately. |
| S4 | **AoI cell id** (= chunk coord) | ✅ §3.4 cell metadata: "AoI cell id (= chunk coord)" | **Accept** | See C9. One grid; the chunk coord is the AoI cell. Server SAD §4.5/§4.7 consume it. |
| S5 | **Zone-line / instance-entrance markers** | ✅ §3.4 cell metadata: "zone-line/instance-entrance markers" | **Accept** | Server needs these for teleport zone transitions (§3.1 zone-local, server-side teleports) and instance entrances (GRP-02). Present. |
| S6 | **PvP / rest flags** | ✅ §3.4 cell metadata: "PvP/rest flags" | **Accept** | Present verbatim in the server payload cell-metadata list. |
| S7 | **Graveyard / leash volume references** (by content id) | ✅ §3.4 cell metadata: "graveyard/leash volume references by content id" | **Accept** | Referenced by content id (not raw path) — consistent with the asset-ID discipline. Serves CMB-03 corpse-run (graveyards) + NPC leash. |
| S8 | **Heightfield extraction fidelity** (adjacent — §5.2 seam) | ✅ §5.2 `export_heightfield(chunk) → f32[129×129]`, zone-local metres, 1 m spacing, sentinel for holes; A-09 §11 "native float32, PASS" | **Accept** | The `ITerrainBackend.export_heightfield` seam (Tools SAD §5.2 op 4) produces the exact `f32[129×129]` grid S1 depends on; Sync Decisions §11 C4 confirms Terrain3D native f32 satisfies it. Hole-sentinel (lint, not silent zero) is a correctness plus. |

---

## Summary

**Client nine-point list:** 6 accept, 3 amend (C2, C4, C5).
**Server cell-metadata needs:** 8 accept, 0 amend.
**Overall: 14 accept, 3 amend.**

### Amendments proposed (all to the §3.3 manifest, all export-time-derivable — low cost)

| Ref | Amendment | Rationale |
|---|---|---|
| **C2** | Document manifest `scene`/`proxy`/`server` refs as **asset-ID (or ID-derived) refs, never raw `res://` paths**. | Holds the IF-8 "no raw path in manifest" rule (Client SAD §5.3 pt 2 / §5.5); today §3.3 leaves the field form unspecified. |
| **C4** | Add optional **`priority`** (ordinal or ground/decoration enum) to `chunks[]`. | The per-chunk priority hint (§5.3 pt 4) is missing; `deps` only covers the shared-dependency half. Lets the streamer order loads. |
| **C5** | Add **`aabb{min,max}`** to `chunks[]`. | Client needs a cheap manifest-level AABB for visibility/priority scoring **before** loading the scene (§5.3 pt 5); the heightfield is a server-payload artifact, not manifest-level. |

**Nature of the amendments:** all three are additive manifest fields, derivable at export from data Forge already has (terrain bounds, placements, priority intent). None touches the coordinate system (§3.1), grid numbers (§3.2), heightfield (§5.2), or the server payload — so none disturbs the A-09 Terrain3D decision or the server-facing slice. They are the "amend" half of an otherwise-accept walk.

**Out-of-format items to route separately (not amendments):**
- Navmesh binary artifacts lack IF-4 registry coverage — **registry-ownership** question (Server SAD §10(b)), not a format defect.
- Migration policy second-half is due M1 start (§3.4) — on schedule, not a walk blocker.

### Ratification checklist (owner ticks)
- [ ] Accept the 14 accept-lines as covered by Tools SAD §3 / §5.2.
- [ ] Rule on **C2** (asset-ID-not-raw-path manifest refs).
- [ ] Rule on **C4** (`priority` field).
- [ ] Rule on **C5** (`aabb` field).
- [ ] Route the navmesh IF-4 registry question (S3 note) to the registry owner.
- [ ] Confirm sign-off recorded in the sync log (this doc is prep only; final format ratified by the owner).
