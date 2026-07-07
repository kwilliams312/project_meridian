#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/build-manifest.sh — emit a nightly build manifest (#62).
#
# Ties the three artifact streams of one nightly into a SINGLE JSON document (the
# "what is this nightly made of" record), validated against
# schema/build-manifest/build-manifest.schema.json:
#
#   * server  — the authd + worldd GHCR images (#175-177): registry, tag, and the
#               immutable per-image digest.
#   * content — the mcc pipeline outputs (#120/#121): the IF-4 <-> IF-5 BLAKE3
#               content_hash (Tools SAD 2.6), the world.sql file checksum, and the
#               pack namespace/version/schema metadata. Read straight out of a
#               content build output dir (scripts/content-build.sh --out DIR).
#   * client  — the Godot client export (#113): the macOS arm64 .app version +
#               export_hash from scripts/export-client-macos.sh. Resolved when
#               --client-version/--client-hash are given (the nightly wires these
#               from the client-export CI artifact); a 'pending' placeholder
#               otherwise (e.g. a locally-generated sample with no export at hand).
#
#   plus the git SHA + a build timestamp.
#
# The content coordinates are NEVER invented: content_hash + pack metadata are
# read from the emitted pack.manifest.json, and this script re-asserts the
# IF-4 (world_manifest) <-> IF-5 (pack.manifest) content-hash tie before emitting
# — a mismatch is a hard error, same gate content-build.sh / content-ci enforce.
#
# The redeploy (#94) consumes the result: `redeploy.sh --manifest <file>` reads
# .server.tag to deploy exactly the tag this manifest pins (see
# docs/ops/artifact-retention.md and deploy/scripts/redeploy.sh).
#
# Usage:
#   scripts/build-manifest.sh --content-out DIR --git-sha SHA [options] > manifest.json
#
# Required:
#   --content-out DIR     Content build output dir (from scripts/content-build.sh).
#   --git-sha SHA         Full 40-hex commit SHA the nightly is cut from.
#
# Options:
#   --kind KIND           nightly | release | adhoc          (default: nightly)
#   --git-ref REF         Git ref, e.g. refs/heads/main.
#   --registry PREFIX     GHCR image path prefix.
#                         (default: ghcr.io/kwilliams312/project_meridian)
#   --tag TAG             Server image tag. (default: the git short SHA)
#   --authd-digest D      authd image digest, sha256:... (default: null / unresolved)
#   --worldd-digest D     worldd image digest, sha256:...  (default: null / unresolved)
#   --pack-ns NS          Pack namespace under pck/meridian/. (default: core)
#   --client-status S     pending | resolved                (default: pending)
#   --client-version V    Client export version (implies --client-status resolved).
#   --client-hash H       Client export hash    (implies --client-status resolved).
#   --built-at TS         RFC 3339 UTC timestamp. (default: now, or SOURCE_DATE_EPOCH)
#   --workflow N          Provenance: workflow name.
#   --run-id ID           Provenance: run id.
#   --run-url URL         Provenance: run url.
#   --out FILE            Write to FILE instead of stdout.
#   --validate            Validate the emitted manifest against the JSON schema
#                         (needs `uv` + the jsonschema dep; no-op warning if absent).
#   -h | --help
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCHEMA="$REPO_ROOT/schema/build-manifest/build-manifest.schema.json"

die() { printf 'build-manifest: %s\n' "$*" >&2; exit 1; }

command -v jq >/dev/null 2>&1 || die "jq not found (required)"

# --- Defaults + arg parsing. -------------------------------------------------
CONTENT_OUT=""
GIT_SHA=""
GIT_REF=""
KIND="nightly"
REGISTRY="ghcr.io/kwilliams312/project_meridian"
TAG=""
AUTHD_DIGEST=""
WORLDD_DIGEST=""
PACK_NS="core"
CLIENT_STATUS="pending"
CLIENT_VERSION=""
CLIENT_HASH=""
BUILT_AT=""
WORKFLOW=""
RUN_ID=""
RUN_URL=""
OUT=""
VALIDATE=0

while [ $# -gt 0 ]; do
  case "$1" in
    --content-out)   CONTENT_OUT="$2"; shift 2 ;;
    --git-sha)       GIT_SHA="$2"; shift 2 ;;
    --git-ref)       GIT_REF="$2"; shift 2 ;;
    --kind)          KIND="$2"; shift 2 ;;
    --registry)      REGISTRY="$2"; shift 2 ;;
    --tag)           TAG="$2"; shift 2 ;;
    --authd-digest)  AUTHD_DIGEST="$2"; shift 2 ;;
    --worldd-digest) WORLDD_DIGEST="$2"; shift 2 ;;
    --pack-ns)       PACK_NS="$2"; shift 2 ;;
    --client-status) CLIENT_STATUS="$2"; shift 2 ;;
    --client-version) CLIENT_VERSION="$2"; CLIENT_STATUS="resolved"; shift 2 ;;
    --client-hash)   CLIENT_HASH="$2"; CLIENT_STATUS="resolved"; shift 2 ;;
    --built-at)      BUILT_AT="$2"; shift 2 ;;
    --workflow)      WORKFLOW="$2"; shift 2 ;;
    --run-id)        RUN_ID="$2"; shift 2 ;;
    --run-url)       RUN_URL="$2"; shift 2 ;;
    --out)           OUT="$2"; shift 2 ;;
    --validate)      VALIDATE=1; shift ;;
    -h|--help)       sed -n '2,70p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown argument: $1 (see --help)" ;;
  esac
done

[ -n "$CONTENT_OUT" ] || die "--content-out is required"
[ -d "$CONTENT_OUT" ] || die "content-out dir not found: $CONTENT_OUT"
[ -n "$GIT_SHA" ]     || die "--git-sha is required"
printf '%s' "$GIT_SHA" | grep -Eq '^[0-9a-f]{40}$' || die "--git-sha must be a full 40-hex SHA (got '$GIT_SHA')"

SHORT_SHA="${GIT_SHA:0:7}"
[ -n "$TAG" ] || TAG="$SHORT_SHA"

# --- Timestamp (reproducible when SOURCE_DATE_EPOCH is set). -----------------
if [ -z "$BUILT_AT" ]; then
  if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
    BUILT_AT="$(date -u -r "${SOURCE_DATE_EPOCH}" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null \
                || date -u -d "@${SOURCE_DATE_EPOCH}" +%Y-%m-%dT%H:%M:%SZ)"
  else
    BUILT_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  fi
fi

# --- Content coordinates (read from the emitted artifacts; never invented). --
WORLD_SQL="$CONTENT_OUT/world.sql"
PACK_MANIFEST="$CONTENT_OUT/pck/meridian/${PACK_NS}/pack.manifest.json"
[ -f "$WORLD_SQL" ]     || die "world.sql not found at $WORLD_SQL"
[ -f "$PACK_MANIFEST" ] || die "pack.manifest.json not found at $PACK_MANIFEST"

# Pack metadata straight from pack.manifest.json.
PACK_VERSION="$(jq -r '.version'                 "$PACK_MANIFEST")"
CONTENT_SCHEMA_VERSION="$(jq -r '.content_schema_version' "$PACK_MANIFEST")"
MCC_VERSION="$(jq -r '.mcc_version'              "$PACK_MANIFEST")"
PACK_HASH="$(jq -r '.content_hash'               "$PACK_MANIFEST")"

# IF-4 world_manifest content_hash (the sole 64-hex token in the INSERT tuple).
SQL_HASH="$(grep -A3 'INSERT INTO world_manifest' "$WORLD_SQL" \
             | grep -Eo "'[0-9a-f]{64}'" | head -1 | tr -d "'")"
[ -n "$SQL_HASH" ]  || die "could not extract IF-4 world_manifest content_hash from $WORLD_SQL"
[ -n "$PACK_HASH" ] || die "could not read IF-5 pack.manifest content_hash from $PACK_MANIFEST"

# The IF-4 <-> IF-5 tie — hard gate before we pin anything.
[ "$SQL_HASH" = "$PACK_HASH" ] \
  || die "CROSS-EMIT TIE FAILED: IF-4 ($SQL_HASH) != IF-5 ($PACK_HASH) — refusing to pin an incoherent nightly"

# world.sql file checksum (transport integrity).
if command -v sha256sum >/dev/null 2>&1; then
  WORLD_SQL_SHA256="$(sha256sum "$WORLD_SQL" | awk '{print $1}')"
else
  WORLD_SQL_SHA256="$(shasum -a 256 "$WORLD_SQL" | awk '{print $1}')"
fi

# --- Digest normalisation (empty => JSON null). ------------------------------
authd_digest_json="null";  [ -n "$AUTHD_DIGEST" ]  && authd_digest_json="$(jq -Rn --arg d "$AUTHD_DIGEST" '$d')"
worldd_digest_json="null"; [ -n "$WORLDD_DIGEST" ] && worldd_digest_json="$(jq -Rn --arg d "$WORLDD_DIGEST" '$d')"
client_version_json="null"; [ -n "$CLIENT_VERSION" ] && client_version_json="$(jq -Rn --arg v "$CLIENT_VERSION" '$v')"
client_hash_json="null";    [ -n "$CLIENT_HASH" ]    && client_hash_json="$(jq -Rn --arg h "$CLIENT_HASH" '$h')"

if [ "$CLIENT_STATUS" = "resolved" ]; then
  CLIENT_NOTE="client export pinned (#113)"
else
  CLIENT_NOTE="client export CI (#113) not wired yet; coordinate reserved"
fi

# --- Assemble the manifest. --------------------------------------------------
MANIFEST="$(jq -n \
  --arg kind "$KIND" \
  --arg built_at "$BUILT_AT" \
  --arg sha "$GIT_SHA" \
  --arg short_sha "$SHORT_SHA" \
  --arg ref "$GIT_REF" \
  --arg registry "$REGISTRY" \
  --arg tag "$TAG" \
  --argjson authd_digest "$authd_digest_json" \
  --argjson worldd_digest "$worldd_digest_json" \
  --arg content_hash "$PACK_HASH" \
  --arg pack_ns "$PACK_NS" \
  --arg pack_version "$PACK_VERSION" \
  --argjson content_schema_version "$CONTENT_SCHEMA_VERSION" \
  --arg mcc_version "$MCC_VERSION" \
  --arg world_sql_sha256 "$WORLD_SQL_SHA256" \
  --arg pack_ns_path "pck/meridian/${PACK_NS}/pack.manifest.json" \
  --arg client_status "$CLIENT_STATUS" \
  --argjson client_version "$client_version_json" \
  --argjson client_hash "$client_hash_json" \
  --arg client_note "$CLIENT_NOTE" \
  --arg workflow "$WORKFLOW" \
  --arg run_id "$RUN_ID" \
  --arg run_url "$RUN_URL" \
  '
  {
    schema: "meridian/build-manifest@1",
    kind: $kind,
    built_at: $built_at,
    git: ({ sha: $sha, short_sha: $short_sha } + (if $ref == "" then {} else { ref: $ref } end)),
    server: {
      registry: $registry,
      tag: $tag,
      images: [
        { name: "authd",  image: ($registry + "/authd"),  tag: $tag, digest: $authd_digest },
        { name: "worldd", image: ($registry + "/worldd"), tag: $tag, digest: $worldd_digest }
      ]
    },
    content: {
      hash_algo: "blake3",
      content_hash: $content_hash,
      pack_namespace: $pack_ns,
      pack_version: $pack_version,
      content_schema_version: $content_schema_version,
      mcc_version: $mcc_version,
      world_sql: { path: "world.sql", sha256: $world_sql_sha256 },
      pack_manifest: { path: $pack_ns_path, content_hash: $content_hash }
    },
    client: {
      status: $client_status,
      version: $client_version,
      export_hash: $client_hash,
      note: $client_note
    }
  }
  + (if ($workflow == "" and $run_id == "" and $run_url == "") then {}
     else { provenance: (
        (if $workflow == "" then {} else { workflow: $workflow } end)
      + (if $run_id   == "" then {} else { run_id: $run_id } end)
      + (if $run_url  == "" then {} else { run_url: $run_url } end)
     ) } end)
  ')"

# --- Optional schema validation. ---------------------------------------------
if [ "$VALIDATE" -eq 1 ]; then
  if command -v uv >/dev/null 2>&1; then
    printf '%s' "$MANIFEST" | uv run --project "$REPO_ROOT" python -c '
import json, sys, jsonschema
schema = json.load(open(sys.argv[1]))
inst = json.load(sys.stdin)
jsonschema.validate(inst, schema)
print("build-manifest: schema OK", file=sys.stderr)
' "$SCHEMA" || die "manifest failed schema validation"
  else
    printf 'build-manifest: WARNING: uv not found — skipping --validate\n' >&2
  fi
fi

# --- Emit. -------------------------------------------------------------------
if [ -n "$OUT" ]; then
  printf '%s\n' "$MANIFEST" > "$OUT"
  printf 'build-manifest: wrote %s\n' "$OUT" >&2
else
  printf '%s\n' "$MANIFEST"
fi
