<!-- SPDX-License-Identifier: Apache-2.0 -->
# Nightly build manifest + artifact-retention policy (#62)

The nightly pipeline produces **three artifact streams** — server images, client
export, and mcc content artifacts. This document defines:

1. The **nightly build manifest** — the one record that pins, for a given
   nightly, the exact coordinates of all three streams (the "what is this nightly
   made of" contract the test-realm redeploy #94 consumes).
2. The **artifact-retention policy** — how long each artifact is kept, and where
   that is enforced.

- **Schema:** [`schema/build-manifest/build-manifest.schema.json`](../../schema/build-manifest/build-manifest.schema.json) — JSON Schema (draft 2020-12).
- **Sample:** [`schema/build-manifest/build-manifest.sample.json`](../../schema/build-manifest/build-manifest.sample.json) — a real manifest generated from the current repo state.
- **Generator:** [`scripts/build-manifest.sh`](../../scripts/build-manifest.sh) — emits + validates the manifest.
- **Workflow:** [`.github/workflows/nightly-manifest.yml`](../../.github/workflows/nightly-manifest.yml) — builds/collects the streams and uploads the manifest nightly.
- **Consumer:** [`.github/workflows/nightly-redeploy.yml`](../../.github/workflows/nightly-redeploy.yml) + [`deploy/scripts/redeploy.sh`](../../deploy/scripts/redeploy.sh) `--manifest` (#94).

---

## 1. The build manifest

One JSON document per nightly, validated against the schema. It pins:

| Stream | Field | Source | Coordinate |
|--------|-------|--------|-----------|
| **server** | `server.images[]` | GHCR (#175-177, `cd.yml`) | `tag` + immutable `digest` (`sha256:…`) for `authd` + `worldd` |
| **content** | `content.content_hash` | mcc `emit-sql`/`emit-pck` (#120/#121) | the IF-4 ↔ IF-5 **BLAKE3** content hash (Tools SAD §2.6) |
| **content** | `content.world_sql.sha256` | mcc `emit-sql` (#120) | SHA-256 file checksum of `world.sql` (transport integrity) |
| **content** | `content.pack_manifest.content_hash` | mcc `emit-pck` (#121) | pack manifest hash — **must equal** `content.content_hash` |
| **client** | `client.*` | client-export CI (#113) | version + hash — **`pending` placeholder until #113 lands** |
| **source** | `git.sha` / `git.short_sha` | `github.sha` | the commit the nightly was cut from |
| **when** | `built_at` | build time | RFC 3339 UTC timestamp |

The content coordinates are **never invented**: `build-manifest.sh` reads them out
of the mcc-emitted `pack.manifest.json` / `world.sql` and **re-asserts the IF-4
(`world_manifest.content_hash`) ↔ IF-5 (`pack.manifest.content_hash`) tie** before
pinning — the same gate `scripts/content-build.sh` and `content-ci.yml` enforce. A
mismatch fails the build; an incoherent nightly is never recorded.

### Generating one locally

```bash
# 1. Build the content stream (mcc emit-sql + emit-pck).
scripts/content-build.sh --out build/content-out

# 2. Emit + validate the manifest for the current commit.
scripts/build-manifest.sh \
  --content-out build/content-out \
  --git-sha "$(git rev-parse HEAD)" \
  --git-ref refs/heads/main \
  --validate
```

Server `digest`s are `null` when generated with no GHCR access (the nightly
workflow resolves them via `docker buildx imagetools inspect`). The `client`
coordinate stays `pending` until #113's client-export CI supplies
`--client-version` / `--client-hash`.

### How the redeploy (#94) consumes it

The field mapping is deliberately narrow — the redeploy only needs to know **which
tag** is the coherent nightly:

```
build-manifest.json  .server.tag   ──►  redeploy.sh --manifest  ──►  MERIDIAN_TAG
                     .content.content_hash  (logged for provenance)
```

`nightly-redeploy.yml` downloads the latest `nightly-build-manifest` artifact
(`gh run download`) and passes it as `MANIFEST` to `redeploy.sh`, which reads
`.server.tag` and deploys exactly that tag instead of a bare `latest`. This is
best-effort and backward-compatible: if no manifest exists yet, the redeploy falls
back to its normal `MERIDIAN_TAG` selection. An explicit `--tag=` / dispatch tag
input always wins over the manifest.

---

## 2. Artifact-retention policy

Retention is enforced two ways: GitHub Actions `retention-days` on each
`upload-artifact` (per-artifact override of the repo default), and — for GHCR
images — the immutable per-SHA tagging convention plus the manual cleanup below.

### GitHub Actions artifacts

| Artifact | Produced by | Retention | Rationale |
|----------|-------------|-----------|-----------|
| `nightly-build-manifest` | `nightly-manifest.yml` (#62) | **90 days** | Tiny JSON; long window gives ample audit/rollback headroom — you can always answer "what was the realm running on night N?" for a quarter. |
| `nightly-content` (`world.sql` + pack) | `nightly-manifest.yml` (#62) | **30 days** | Larger, and reproducible from the pinned `git.sha`; 30 days covers the practical redeploy/rollback window. |
| `it-m0-content-build` | `content-ci.yml` (#123) | repo default (90 days) | CI content build output for downstream consumers. |
| `sbom-{authd,worldd}` | `cd.yml` (#180) / `release.yml` (#179) | **30 days** | Supply-chain SBOMs; the signed cosign attestation on the image digest is the durable record, so the artifact copy is a convenience. |
| `digest-{authd,worldd}` | `release.yml` (#179) | **1 day** | Ephemeral inter-job hand-off only. |

**Policy for new nightly artifacts:** every `upload-artifact` step **must** set an
explicit `retention-days` (do not rely on the repo default) and
`if-no-files-found: error`. Manifests: **90 days**. Bulky reproducible build
outputs: **30 days**.

### GHCR container images (server stream)

Images (`ghcr.io/kwilliams312/project_meridian/{authd,worldd}`) are published by
`cd.yml` (#175) tagged by **immutable short-SHA** + the mutable `latest`. They are
**not** auto-expired today — the build manifest references them by immutable
`digest`, so a pinned nightly stays reproducible as long as the digest exists.

**Cleanup guidance (manual / future automation):**

- **Keep:** every tag referenced by a manifest still inside its retention window
  (≤ 90 days), all release-tagged (`vX.Y.Z`) images, and `latest`.
- **Prune candidates:** untagged (dangling) manifests and short-SHA images older
  than 90 days with no referencing manifest.
- A future scheduled job may enforce this via
  `actions/delete-package-versions` (keep-N / older-than); until then, prune via
  the GHCR package UI during ops review. Never delete a digest a retained manifest
  still pins.

### Summary

| Stream | Immutable identity | Kept | Where enforced |
|--------|--------------------|------|----------------|
| Server images | OCI `digest` | 90 days (manifest-referenced) | GHCR + manual/`delete-package-versions` |
| Content | BLAKE3 `content_hash` | 30 days (artifact) / ∞ (reproducible from git SHA) | `nightly-manifest.yml` `retention-days` |
| Client export | export hash (#113) | TBD with #113 | client-export CI |
| **Manifest (the tie)** | git SHA + `built_at` | **90 days** | `nightly-manifest.yml` `retention-days` |

---

## Follow-ups

- **#113 — client-export CI:** wire the real Godot export version + hash into the
  `client` coordinate (`--client-version` / `--client-hash`). The schema slot
  already exists (`status: pending` → `resolved`), so this is a fill-in, not a
  schema change.
- **GHCR pruning automation:** a scheduled `delete-package-versions` job honoring
  the keep/prune rules above.
