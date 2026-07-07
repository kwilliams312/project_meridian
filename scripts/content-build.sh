#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/content-build.sh — the repeatable, deterministic IT-M0 content build (#123).
#
# One command that turns the Codex-authored /content tree into the two mcc
# artifacts the IT-M0 "content visible via the mcc pipe" step needs
# (docs/it-m0-runbook.md §5, DC-10/DC-11):
#
#   1. builds mcc (the Meridian Content Compiler, tools/mcc),
#   2. runs the full `mcc build content` pipeline
#      (discover → parse → validate → link → emit-sql → emit-pck), and
#   3. collects the artifacts into a stable output dir:
#        <out>/world.sql                              — IF-4 world DB SQL + world_manifest
#        <out>/pck/meridian/<ns>/pack.manifest.json   — IF-5 client pack manifest
#        <out>/pck/meridian/<ns>/pack.contents.jsonl  — IF-5 M0 directory-manifest pack
#
# It then asserts the two cross-emit invariants that make the pipe trustworthy:
#   * the IF-4 world_manifest content_hash == the IF-5 pack.manifest content_hash
#     (the three-way tie, Tools SAD §2.6), and
#   * determinism — same source + same mcc ⇒ byte-identical artifacts on rebuild
#     (Tools SAD §5; skip with --no-determinism-check for a fast single build).
#
# Consuming the artifacts (both come straight from mcc — ZERO hand-written SQL):
#   * world.sql → the world DB. Load the world DDL (schema/sql/world/*.sql) then
#     this world.sql into the `meridian_world` MariaDB. In a deploy this is the
#     read-only, mcc-produced world artifact worldd boots from and verifies via
#     world_manifest (deploy/docker/db-init/03-world.sql loads the DDL; the
#     nightly build replaces the data wholesale). Local load:
#       mariadb ... meridian_world < <(cat schema/sql/world/*.sql)   # DDL (once)
#       mariadb ... meridian_world < <out>/world.sql                 # mcc data
#   * pck/ → the client. The client mounts pack.manifest.json (IF-5, issue #107)
#     and verifies each resource against its per-resource BLAKE3, keyed by IF-9
#     numeric id. pack.contents.jsonl is the M0 stand-in for the Godot-native .pck.
#
# Usage:
#   scripts/content-build.sh                     # build mcc + content, verify
#   scripts/content-build.sh --out DIR           # artifact dir (default: build/content-out)
#   scripts/content-build.sh --content DIR       # content root (default: content)
#   scripts/content-build.sh --built-at "<ts>"   # world_manifest.built_at (default: fixed epoch)
#   scripts/content-build.sh --no-determinism-check   # single build, skip the rebuild compare
#   scripts/content-build.sh --skip-mcc-build    # reuse an existing tools/mcc/build/mcc
#   scripts/content-build.sh --help
set -euo pipefail

# --- Repo root (one level up from scripts/). ---------------------------------
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# --- Logging (color when attached to a TTY). ---------------------------------
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  _R=$'\033[0m'; _B=$'\033[1m'; _BLU=$'\033[34m'; _GRN=$'\033[32m'; _YEL=$'\033[33m'; _RED=$'\033[31m'
else
  _R=''; _B=''; _BLU=''; _GRN=''; _YEL=''; _RED=''
fi
log()  { printf '%s==>%s %s\n' "${_BLU}${_B}" "${_R}" "$*"; }
ok()   { printf '%s  OK%s %s\n' "${_GRN}" "${_R}" "$*"; }
warn() { printf '%s  ! %s%s\n' "${_YEL}" "$*" "${_R}" >&2; }
die()  { printf '%s  X %s%s\n' "${_RED}${_B}" "$*" "${_R}" >&2; exit 1; }

# --- Defaults + arg parsing. -------------------------------------------------
OUT_DIR="build/content-out"
CONTENT_DIR="content"
BUILT_AT=""                 # empty → mcc's reproducible fixed epoch
DETERMINISM_CHECK=1
SKIP_MCC_BUILD=0

while [ $# -gt 0 ]; do
  case "$1" in
    --out)                  OUT_DIR="$2"; shift 2 ;;
    --content)              CONTENT_DIR="$2"; shift 2 ;;
    --built-at)             BUILT_AT="$2"; shift 2 ;;
    --no-determinism-check) DETERMINISM_CHECK=0; shift ;;
    --skip-mcc-build)       SKIP_MCC_BUILD=1; shift ;;
    -h|--help)
      sed -n '2,45p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) die "unknown argument: $1 (see --help)" ;;
  esac
done

MCC_BUILD_DIR="tools/mcc/build"
MCC="${MCC_BUILD_DIR}/mcc"
PACK_NS="core"   # the single M0 pack namespace (content/core/pack.yaml)

# --- 1. Build mcc. -----------------------------------------------------------
# yaml-cpp is resolved by mcc's own CMake: system package if present, else a
# pinned FetchContent fallback (0.8.0). No content-build-specific deps.
if [ "$SKIP_MCC_BUILD" -eq 1 ] && [ -x "$MCC" ]; then
  log "Reusing existing mcc at $MCC (--skip-mcc-build)"
else
  command -v cmake >/dev/null 2>&1 || die "cmake not found (macOS: brew install cmake; see scripts/dev/setup-macos.sh)"
  log "Building mcc (Release) → $MCC"
  # -G Ninja when available (matches CI); otherwise the platform default generator.
  if command -v ninja >/dev/null 2>&1; then
    cmake -S tools/mcc -B "$MCC_BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  else
    cmake -S tools/mcc -B "$MCC_BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
  fi
  cmake --build "$MCC_BUILD_DIR" --target mcc >/dev/null
fi
[ -x "$MCC" ] || die "mcc binary not found at $MCC after build"
ok "mcc: $("$MCC" --version)"

# --- 2. Run the full content build. ------------------------------------------
# `mcc build content` writes CWD-relative: build/world.sql and
# build/pck/meridian/<ns>/. We run it in a scratch dir so it never collides with
# CMake build trees, then collect into $OUT_DIR. build/ is gitignored.
SCRATCH="build/.content-build-scratch"
rm -rf "$SCRATCH"
mkdir -p "$SCRATCH"

built_at_args=()
[ -n "$BUILT_AT" ] && built_at_args=(--built-at "$BUILT_AT")

run_build() {  # $1 = scratch dir to build into
  local dir="$1"
  # ${arr[@]:+...} guards the empty-array expansion under `set -u` on bash 3.2 (macOS).
  ( cd "$dir" \
      && "$REPO_ROOT/$MCC" emit-sql "$REPO_ROOT/$CONTENT_DIR" --out build/world.sql ${built_at_args[@]:+"${built_at_args[@]}"} >/dev/null \
      && "$REPO_ROOT/$MCC" emit-pck "$REPO_ROOT/$CONTENT_DIR" --out build/pck ${built_at_args[@]:+"${built_at_args[@]}"} >/dev/null )
}

# `mcc build content` allocates ids (writes idmap.lock) then emits. The committed
# idmap.lock is already up to date (CI's read-only drift-check proves it), so we
# emit read-only against it — same artifacts, no working-tree churn. We call the
# emit stages directly (build's front half is check+link+emit; the emit stages
# each re-run discover→parse→validate→link read-only, so the artifacts are
# identical to `mcc build`'s while leaving idmap.lock untouched).
log "Running mcc content pipeline (validate → link → emit-sql → emit-pck)"
run_build "$SCRATCH"

WORLD_SQL="$SCRATCH/build/world.sql"
PACK_MANIFEST="$SCRATCH/build/pck/meridian/${PACK_NS}/pack.manifest.json"
PACK_CONTENTS="$SCRATCH/build/pck/meridian/${PACK_NS}/pack.contents.jsonl"
[ -f "$WORLD_SQL" ]     || die "emit-sql produced no world.sql at $WORLD_SQL"
[ -f "$PACK_MANIFEST" ] || die "emit-pck produced no pack.manifest.json at $PACK_MANIFEST"
[ -f "$PACK_CONTENTS" ] || die "emit-pck produced no pack.contents.jsonl at $PACK_CONTENTS"
ok "emit-sql → world.sql, emit-pck → pack.manifest.json + pack.contents.jsonl"

# --- 3. Collect artifacts into the stable output dir. ------------------------
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/pck/meridian/${PACK_NS}"
cp "$WORLD_SQL"     "$OUT_DIR/world.sql"
cp "$PACK_MANIFEST" "$OUT_DIR/pck/meridian/${PACK_NS}/pack.manifest.json"
cp "$PACK_CONTENTS" "$OUT_DIR/pck/meridian/${PACK_NS}/pack.contents.jsonl"
log "Artifacts collected under $OUT_DIR:"
printf '      %s\n' \
  "world.sql                              (IF-4 world DB SQL + world_manifest)" \
  "pck/meridian/${PACK_NS}/pack.manifest.json   (IF-5 client pack manifest)" \
  "pck/meridian/${PACK_NS}/pack.contents.jsonl  (IF-5 M0 directory-manifest pack)"

# --- 4. Invariant: IF-4 world_manifest hash == IF-5 pack.manifest hash. ------
# world.sql row is: (pack_namespace, pack_version, id_band, content_hash, ...);
# content_hash is the sole 64-hex token in the world_manifest INSERT tuple.
sql_hash="$(grep -A3 'INSERT INTO world_manifest' "$OUT_DIR/world.sql" \
             | grep -Eo "'[0-9a-f]{64}'" | head -1 | tr -d "'")"
pck_hash="$(grep -Eo '"content_hash"[[:space:]]*:[[:space:]]*"[0-9a-f]{64}"' \
             "$OUT_DIR/pck/meridian/${PACK_NS}/pack.manifest.json" \
             | grep -Eo '[0-9a-f]{64}' | head -1)"
[ -n "$sql_hash" ] || die "could not extract IF-4 world_manifest content_hash from world.sql"
[ -n "$pck_hash" ] || die "could not extract IF-5 pack.manifest content_hash from pack.manifest.json"
if [ "$sql_hash" != "$pck_hash" ]; then
  die "CROSS-EMIT INVARIANT FAILED: IF-4 ($sql_hash) != IF-5 ($pck_hash)"
fi
ok "IF-4 world_manifest hash == IF-5 pack.manifest hash: $sql_hash"

# --- 5. Determinism: rebuild in a second scratch, compare byte-for-byte. ------
if [ "$DETERMINISM_CHECK" -eq 1 ]; then
  log "Determinism check: rebuilding and comparing artifacts byte-for-byte"
  SCRATCH2="build/.content-build-scratch-2"
  rm -rf "$SCRATCH2"; mkdir -p "$SCRATCH2"
  run_build "$SCRATCH2"
  for rel in "build/world.sql" \
             "build/pck/meridian/${PACK_NS}/pack.manifest.json" \
             "build/pck/meridian/${PACK_NS}/pack.contents.jsonl"; do
    if ! cmp -s "$SCRATCH/$rel" "$SCRATCH2/$rel"; then
      die "NON-DETERMINISTIC: $rel differs between two builds"
    fi
  done
  rm -rf "$SCRATCH2"
  ok "Deterministic: two builds produced byte-identical world.sql + pack artifacts"
else
  warn "Determinism check skipped (--no-determinism-check)"
fi

rm -rf "$SCRATCH"
log "${_B}Content build complete.${_R} Artifacts in $OUT_DIR"
