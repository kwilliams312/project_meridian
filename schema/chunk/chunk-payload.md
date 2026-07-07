# Zone Chunk Format v1 (IF-6) — payload & manifest

The terrain/zone streaming contract the **Tools track (Forge) owns** and the **Client** (runtime
streamer) and **Server** (`worldd`, cell/nav metadata) consume. This directory is the contract
that discharges the **format half of A-08** (Tools SAD §10.3 pt 7 — A-08 signs when this merges
with Client + Server approvals).

- **[`chunk-manifest.schema.yaml`](chunk-manifest.schema.yaml)** — the per-zone grid index
  (`<zone>.chunks.json`), JSON Schema draft 2020-12. Tools SAD **§3.3**.
- **[`chunk.fbs`](chunk.fbs)** — the per-chunk **server** payload (`<cx>_<cz>.chunk.bin`),
  FlatBuffers. Tools SAD **§3.4** (server slice), grid numbers **§3.2**.
- **This file** — the human-facing narrative: layout, endianness, coordinates, sentinels,
  determinism, and how the manifest and the two payloads relate.

Authoritative sources: Tools SAD [§3.1–§3.4 and §5.2](../../docs/sad/tools-sad.md); the **A-08
requirements walk** [`docs/reviews/a08-terrain-walk.md`](../../docs/reviews/a08-terrain-walk.md)
(14 accept / 3 amend — C2, C4, C5); the A-09 terrain decision
([Sync Decisions §11](../../docs/01-SYNC-DECISIONS.md), fork-and-vendor Terrain3D).

> **Status: v1 contract for A-08 sign-off.** Consumed at **M1** (chunk ingestion / navmesh bake —
> Tools SAD §8); **M0 lands the contract only** and runs on a flat bootstrap map (D-19), so no
> runtime parses these files at M0.

---

## Two payloads, split on the consumer line

Per Tools SAD §3.4 each **existing** chunk has exactly two artifacts, split precisely on the
client/server boundary — the server never parses Forge scene files:

| Artifact | Format | Consumer | Contract |
|---|---|---|---|
| `…/chunks/<cx>_<cz>.scn` (+ `.proxy.scn`) | compiled Godot scene | **Client** streamer (into the IF-5 `.pck`) | Tools SAD §3.4 client payload — **not** schema'd here (it is a Godot resource) |
| `<zone>/<cx>_<cz>.chunk.bin` | FlatBuffers (`chunk.fbs`) | **Server** `worldd` (via the IF-4 artifact set; `mcc` bakes it) | `chunk.fbs`, this directory |

The **manifest** (`<zone>.chunks.json`) is the index over both: one sparse entry per existing
chunk (holes allowed), carrying the grid definition and per-chunk refs + metadata.

```
<zone>.chunks.json            ── manifest (chunk-manifest.schema.yaml)
  └─ chunks[] ── each entry ──▶ scene   (ID-derived ref → client .scn)     ┐ resolved to res://
                               proxy   (ID-derived ref → proxy .scn|null)  │ at load (SAD §4.2);
                               server  (ID-derived ref → .chunk.bin)       ┘ never a raw path (C2)
                               hash    (blake3 over BOTH payloads)
                               aabb, priority, deps
```

`zone.schema.yaml`'s reserved `chunk_manifest` field points at the manifest once A-08 signs.

---

## Coordinate system (Tools SAD §3.1, D-20)

- **Zone-local metres, permanently.** Every zone is its own map/origin; zone transitions are
  server-side teleports (no global world-space stitching in 1.0). This keeps float precision
  bounded on both the Godot client and the server physics/AoI path.
- **Godot axes:** right-handed, **Y-up**, **X east**, **−Z north**. The chunk grid tiles the
  **XZ plane**; heights are along **Y**. The server adopts these axes as-is — no per-consumer
  axis flips (walk S-axes / §3.1).
- **Chunk indices** `(cx, cz)` may be **negative** (Zone-01 spawns at x ≈ −300). World→cell:
  `cx = floor((x − origin.x) / chunk_size_m)`, likewise `cz` (walk C1). The `(cx, cz)` pair **is**
  the server AoI cell id (walk C9/S4 — one grid, both consumers).

## Grid & headline numbers (Tools SAD §3.2)

| Parameter | v1 value | Where |
|---|---|---|
| Chunk size | **128 m × 128 m** | manifest `chunk_size_m` (a **field**, not a constant — TS-3) |
| Heightfield | **129×129** samples, **1 m** spacing, **f32**, **row-major**, shared-edge | `chunk.fbs` `Heightfield` |
| Navmesh tile | **32 m** ⇒ **4×4 = 16** tiles/chunk (Detour serialized) | `chunk.fbs` `NavmeshTile` |
| Far ring | default **6** (unloaded beyond; terrain clipmap only) | manifest `far_ring` |

The 129th row/column is the **shared edge** — it duplicates the neighbouring chunk's first
row/column so adjacent heightfields join seamlessly. Samples are **row-major**: index
`i = z * 129 + x`, `z` outer (north–south), `x` inner (east–west).

---

## Server payload binary layout (`chunk.fbs`)

- **FlatBuffers**, root table `ServerChunk`, `file_identifier "MCHK"`, on-disk name
  `<cx>_<cz>.chunk.bin`.
- **Endianness: little-endian.** FlatBuffers is little-endian on the wire on every platform
  (matches the IF-1/IF-2 `u32 LE` framing in `schema/net`); no per-host byte-swap.
- **`format_version` (uint16) is the first field** of `ServerChunk`. Loaders read it (after the
  4-byte `MCHK` magic) and **reject unknown majors** before any further access (Tools SAD §3.4).
- **Reading:** every inbound buffer must pass the FlatBuffers **verifier** before access (same
  discipline as `schema/net`). No hand-written parser; `flatc`/`mcc` generate the bindings.

### `ServerChunk` contents (Tools SAD §3.4 server slice)

| Field | Meaning | Walk ref |
|---|---|---|
| `format_version` | major version; reject-unknown | C7 |
| `coord` | chunk index = AoI cell id | C9 / S4 |
| `heightfield` | 129×129 f32 movement grid (§3.2) | S1 |
| `hf_collider` | terrain heightfield collider params | S1 |
| `static_colliders[]` | collision-relevant kit placements (shape + transform; `dressing-only` excluded) | §3.4 |
| `navmesh_tiles[]` | 4×4 Detour tiles + `recast_rev` | S3 |
| `cell_flags` | PvP / rest bitfield | S6 |
| `liquids[]` | liquid regions (type + surface height + bounds) | S2 |
| `markers[]` | zone-line / instance-entrance markers (→ content id) | S5 |
| `volume_refs[]` | graveyard / leash volumes **by content id** | S7 |

### Heightfield hole sentinel

`export_heightfield` surfaces holes / un-sculpted cells as a **sentinel** the exporter lints on,
rather than silently zeroing (Tools SAD §5.2 op 4). **v1 sentinel = IEEE-754 quiet `NaN`** — the
natural "no data" float, detectable with `isnan` and never a legitimate height. A well-formed
shipped chunk contains **no** sentinels (the export lint is a hard gate); the value exists so a
partial/in-progress bake is detectable rather than lying with `0.0`.
*(The SAD specifies "a sentinel" without pinning the value; `NaN` is this contract's concrete
choice — flagged for sign-off.)*

---

## Manifest ↔ payload relationship

- **`hash`** on each manifest entry is a **BLAKE3-256** (`blake3:<64 hex>`, Tools SAD §10.1)
  covering **both** payloads, so a change to either invalidates the entry for incremental
  rebuilds and pack verification (walk C5/C8).
- **`scene` / `proxy` / `server`** are **asset-ID or ID-derived logical refs, never raw `res://`
  or filesystem paths** (A-08 amendment **C2**). The concrete `res://` resource path is derived
  from the id at load time (Tools SAD §4.2 — "the resource path is derived from the ID"), so Art
  can move/rename source files without breaking the manifest. `proxy: null` is the explicit
  "no proxy" form for a present chunk (amendment **C3**).
- **`aabb`** (amendment **C5**) is a cheap, manifest-level world-space box the client scores
  visibility / load priority from **before** loading the scene.
- **`priority`** (amendment **C4**, optional) is a per-chunk load-order hint; lower loads first.
  Ring/priority policy is otherwise a **client runtime** choice (§3.2).
- **`deps`** lists shared **IF-8 asset ids** so pack installers and the streamer can prefetch.

## Determinism (walk C8; Tools SAD §2.5 / §9.1)

Identical Forge input ⇒ **identical bytes and hashes**. This schema fixes FlatBuffers field order;
the exporter additionally MUST emit every vector in a **fixed, sorted order** (no hash-map
iteration): `static_colliders` and `volume_refs` by their id/content-id; `navmesh_tiles` by
`(tz, tx)`; `liquids` / `markers` by a stable spatial key; manifest `chunks[]` by `(cz, cx)` and
`deps` sorted + de-duplicated. Any nondeterminism is a **P0** — it breaks pack verification
(TLS-08) and the content-hash tie. Confirmed by the double-build hash-compare gate.

## Versioning & migration (Tools SAD §3.4 — A-08 second half, due M1 start)

- **Within a major:** additive FlatBuffers evolution — append fields with defaults; never
  remove/reorder/retype/renumber an existing field; enums are append-only within their backing
  type (same rules as `schema/net/README.md`). A manifest minor add is a new optional property.
- **Major bump = re-export event:** Forge re-exports all zones; loaders keep at most **N−1** read
  support for one milestone. A `format_version` major bump lands a **new** manifest schema file.

---

## Compiling / validating

```sh
# Server payload — generate C++ bindings (as worldd/mcc do; no committed generated code):
flatc --cpp -o build/gen schema/chunk/chunk.fbs

# Manifest schema — meta-schema check + validate a <zone>.chunks.json against it (draft 2020-12):
#   Draft202012Validator.check_schema(...) ; Draft202012Validator(schema).validate(manifest)
```

`chunk.fbs` is **not** wired into the M0 server build (`server/libs/proto` compiles only the
`schema/net` wire contracts) — the server consumes chunks at **M1**. It compiles standalone with
`flatc` today; CI compilation lands with M1 chunk ingestion.
