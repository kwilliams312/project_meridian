#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/check-golden.sh — the mcc determinism gate + golden-corpus check (#122).
#
# GOAL: guarantee mcc emit output is BYTE-DETERMINISTIC across runs and machines,
# by diffing a fresh build of content/core against a checked-in "golden corpus" —
# a frozen snapshot of the expected mcc output (tools/mcc/golden/). Any drift ⇒
# the gate fails with a clear, actionable message. This is the determinism
# guarantee the whole content pipeline (IF-4/IF-5 content hashes, IF-9 idmap)
# rests on: it catches accidental nondeterminism AND unintended content/output
# changes in one gate (Tools SAD §9.1 determinism, §9.4 golden corpus; DC-11).
#
# The golden corpus is FIVE files — the complete expected mcc output for the
# `core` pack under content/, all stamped with a FIXED built_at so they are
# reproducible byte-for-byte:
#
#   tools/mcc/golden/world.sql            — IF-4 world DB SQL + world_manifest
#   tools/mcc/golden/pack.manifest.json   — IF-5 client pack manifest
#   tools/mcc/golden/pack.contents.jsonl  — IF-5 M0 directory-manifest pack
#   tools/mcc/golden/pack.data.json       — IF-5 M0 client-render field data (#477)
#   tools/mcc/golden/index.json           — `mcc index --json` (the IF-9 ID index)
#
# Three independent checks, any failing fails the gate:
#
#   (1) GOLDEN MATCH  — a fresh mcc build of content/ (with the fixed built_at)
#       matches the checked-in golden byte-for-byte. Drift here means either
#       nondeterminism crept in, or content/output changed without the golden
#       being regenerated.
#   (2) CROSS-RUN     — building twice in a row yields byte-identical output.
#       This isolates *nondeterminism* specifically (independent of the golden),
#       so a stale golden and a genuinely-flaky build give distinct diagnoses.
#   (3) STAGED PACK   — the committed client copy of the pack artifacts
#       (client/project/meridian/core/, the res://meridian/core mount the client
#       ships at M0, issue #477) matches the fresh emit byte-for-byte, so the
#       client can never silently ship stale catalog/worn/dye data.
#       --update-golden refreshes golden AND staged copy in lockstep.
#
# REGENERATING THE GOLDEN (when content legitimately changes):
#
#   scripts/check-golden.sh --update-golden
#
# This rebuilds mcc + content with the fixed built_at and overwrites the five
# golden files. Review the golden diff exactly as you would review the content
# diff — a golden change is a content/output change — then commit both together.
#
# Usage:
#   scripts/check-golden.sh                 # run the gate (golden match + cross-run)
#   scripts/check-golden.sh --update-golden # regenerate the checked-in golden
#   scripts/check-golden.sh --skip-mcc-build   # reuse an existing tools/mcc/build/mcc
#   scripts/check-golden.sh --help
#
# The fixed built_at is pinned here (GOLDEN_BUILT_AT) rather than relying on the
# mcc default, so the golden is stable even if the mcc default ever changes.
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

# --- Config. -----------------------------------------------------------------
# The frozen build timestamp baked into world_manifest.built_at + the pack
# manifest. Pinned to the mcc reproducible epoch so the golden is stable and
# machine-independent. Never the wall clock.
GOLDEN_BUILT_AT="1970-01-01 00:00:00"
CONTENT_DIR="content"           # pack root (contains the `core` pack)
PACK_NS="core"
GOLDEN_DIR="tools/mcc/golden"
MCC_BUILD_DIR="tools/mcc/build"
MCC="${MCC_BUILD_DIR}/mcc"

UPDATE_GOLDEN=0
SKIP_MCC_BUILD=0
while [ $# -gt 0 ]; do
  case "$1" in
    --update-golden)  UPDATE_GOLDEN=1; shift ;;
    --skip-mcc-build) SKIP_MCC_BUILD=1; shift ;;
    -h|--help)
      sed -n '2,63p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) die "unknown argument: $1 (see --help)" ;;
  esac
done

# --- 1. Build mcc (unless reusing an existing binary). -----------------------
if [ "$SKIP_MCC_BUILD" -eq 1 ] && [ -x "$MCC" ]; then
  log "Reusing existing mcc at $MCC (--skip-mcc-build)"
else
  command -v cmake >/dev/null 2>&1 || die "cmake not found (macOS: brew install cmake)"
  log "Building mcc (Release) → $MCC"
  if command -v ninja >/dev/null 2>&1; then
    cmake -S tools/mcc -B "$MCC_BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
  else
    cmake -S tools/mcc -B "$MCC_BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
  fi
  cmake --build "$MCC_BUILD_DIR" --target mcc >/dev/null
fi
[ -x "$MCC" ] || die "mcc binary not found at $MCC after build"
ok "mcc: $("$MCC" --version)"

# --- Emit all five artifacts into a target dir (the golden's shape). ---------
# emit-sql / emit-pck each re-run discover→parse→validate→link read-only against
# the committed idmap.lock, so this leaves the working tree untouched (no id
# churn) and is identical to what `mcc build` / content-build.sh emit.
emit_into() {  # $1 = target dir
  local dir="$1"
  mkdir -p "$dir"
  # --pack pins the core pack for BOTH emits so the golden is the core pack's
  # single-pack snapshot: emit-sql emits one world_manifest row + only core content
  # (a second pack's roster rows would otherwise share the world.sql and collide on
  # the per-pack roster_id key on load — design §4, story #770), and emit-pck emits
  # the core pack (a `chibi` pack sorts before `core` and would otherwise hijack the
  # single-pack emit). index.json below is NOT pack-scoped — it is the full IF-9 ID
  # index across every pack, so it still lists chibi's ids.
  "$REPO_ROOT/$MCC" emit-sql "$REPO_ROOT/$CONTENT_DIR" --pack "$PACK_NS" \
      --out "$dir/world.sql" --built-at "$GOLDEN_BUILT_AT" >/dev/null
  "$REPO_ROOT/$MCC" emit-pck "$REPO_ROOT/$CONTENT_DIR" --pack "$PACK_NS" \
      --out "$dir/pck" --built-at "$GOLDEN_BUILT_AT" >/dev/null
  # `mcc index` resolves ids against idmap.lock, so it runs against the pack root
  # ($CONTENT_DIR), not content/core. Its --json output is fully sorted (by type,
  # then by id) → deterministic. Diagnostics go to stderr; only stdout is captured.
  "$REPO_ROOT/$MCC" index "$REPO_ROOT/$CONTENT_DIR" --json > "$dir/index.json"
  # Flatten the pack into the golden's flat shape.
  cp "$dir/pck/meridian/${PACK_NS}/pack.manifest.json"  "$dir/pack.manifest.json"
  cp "$dir/pck/meridian/${PACK_NS}/pack.contents.jsonl" "$dir/pack.contents.jsonl"
  cp "$dir/pck/meridian/${PACK_NS}/pack.data.json"      "$dir/pack.data.json"
  rm -rf "$dir/pck"
}

GOLDEN_FILES=(world.sql pack.manifest.json pack.contents.jsonl pack.data.json index.json)

# The STAGED CLIENT PACK: the committed copy of the three pack artifacts the
# client mounts at res://meridian/core (issue #477 — the M0 stand-in until an
# automated client content-staging step exists). It MUST stay byte-identical to
# the emit (same fixed built_at as the golden). --update-golden refreshes it in
# lockstep; the gate below fails on any drift.
STAGED_DIR="client/project/meridian/core"
STAGED_FILES=(pack.manifest.json pack.contents.jsonl pack.data.json)

# The STAGED MODEL SOURCES (spec ② T3, story #540): the source .glb bytes the
# client's AssembledCharacter loads at runtime via GLTFDocument. emit-pck is
# DECLARATIVE at M0 (no Godot importer runs), so the pack.contents.jsonl res://
# paths (.scn) have no bytes behind them — the assembler falls back to the .glb
# staged NEXT TO the declared path (same by-ID layout, source extension). These
# copies are hand-staged like the pack JSON above and drift-gated the same way:
# byte-identical to their content-tree source, refreshed by --update-golden.
# Entry format: "<content-tree source>:<staged copy under $STAGED_DIR>".
#
# The ardent/male face/skin customization textures (spec ⑤/S1, story #568) are
# staged the same way ahead of the material-application work (⑤/S3, S6) that
# will consume their bytes, so the staged pack never has to play catch-up with
# a batch of new customization assets landing at once.
#
# The Warden's Kit plates + RGB dye masks (spec ⑤/S2, story #569): the assembler
# loads each worn plate .glb via ContentDB.model_path → res://meridian/core/... ,
# and the ⑤/S3 dye shader samples the sibling _mask.png — both need staged bytes,
# or every plate hits _load_model_scene's _fail path (invisible kit). Staged at the
# by-ID layout (id dots → path, source extension). The check_staged_models gate
# below independently fails if a committed model has bytes but no staged copy, so
# a future forgotten entry can't silently ship an unrenderable piece again.
STAGED_ART=(
  "content/core/assets/art/char/sk_ardent_male_base.glb:art/char/ardent/male/base.glb"
  "content/core/assets/art/char/sk_ardent_male_skeleton.glb:art/char/ardent/male/skeleton.glb"
  "content/core/assets/art/char/sk_dolmen_male_base.glb:art/char/dolmen/male/base.glb"
  "content/core/assets/art/char/sk_dolmen_male_skeleton.glb:art/char/dolmen/male/skeleton.glb"
  "content/core/assets/art/item/weapon/pickaxe_rusty.glb:art/item/weapon/pickaxe_rusty.glb"
  "content/core/assets/art/char/ardent/male/face_1_bc.png:art/char/ardent/male/face_1_bc.png"
  "content/core/assets/art/char/ardent/male/face_2_bc.png:art/char/ardent/male/face_2_bc.png"
  "content/core/assets/art/char/ardent/male/face_3_bc.png:art/char/ardent/male/face_3_bc.png"
  "content/core/assets/art/char/ardent/male/face_4_bc.png:art/char/ardent/male/face_4_bc.png"
  "content/core/assets/art/char/ardent/male/skin_1_bc.png:art/char/ardent/male/skin_1_bc.png"
  "content/core/assets/art/char/ardent/male/skin_2_bc.png:art/char/ardent/male/skin_2_bc.png"
  "content/core/assets/art/char/ardent/male/skin_3_bc.png:art/char/ardent/male/skin_3_bc.png"
  "content/core/assets/art/char/ardent/male/hair_1.glb:art/char/ardent/male/hair_1.glb"
  "content/core/assets/art/char/ardent/male/hair_2.glb:art/char/ardent/male/hair_2.glb"
  "content/core/assets/art/char/ardent/male/hair_3.glb:art/char/ardent/male/hair_3.glb"
  "content/core/assets/art/char/ardent/male/hair_4.glb:art/char/ardent/male/hair_4.glb"
  "content/core/assets/art/item/armor/warden_head.glb:art/item/armor/warden_head.glb"
  "content/core/assets/art/item/armor/warden_shoulders.glb:art/item/armor/warden_shoulders.glb"
  "content/core/assets/art/item/armor/warden_chest.glb:art/item/armor/warden_chest.glb"
  "content/core/assets/art/item/armor/warden_hands.glb:art/item/armor/warden_hands.glb"
  "content/core/assets/art/item/armor/warden_legs.glb:art/item/armor/warden_legs.glb"
  "content/core/assets/art/item/armor/warden_feet.glb:art/item/armor/warden_feet.glb"
  "content/core/assets/art/item/armor/warden_head_mask.png:art/item/armor/warden_head_mask.png"
  "content/core/assets/art/item/armor/warden_shoulders_mask.png:art/item/armor/warden_shoulders_mask.png"
  "content/core/assets/art/item/armor/warden_chest_mask.png:art/item/armor/warden_chest_mask.png"
  "content/core/assets/art/item/armor/warden_hands_mask.png:art/item/armor/warden_hands_mask.png"
  "content/core/assets/art/item/armor/warden_legs_mask.png:art/item/armor/warden_legs_mask.png"
  "content/core/assets/art/item/armor/warden_feet_mask.png:art/item/armor/warden_feet_mask.png"
)

# --- The CHIBI staged client pack (story #809). ------------------------------
# The chibi theme pack (content/chibi) is a SECOND single-pack client mount the
# client ships at res://meridian/chibi — the pack the dev realm loads once
# MERIDIAN_REALM_THEME=chibi (C9/#762). It is staged and drift-gated EXACTLY like
# core above, so a clean checkout (and the hosted chibi realm's client) can never
# ship a stale/pre-roster chibi pack or an unrenderable body:
#   * the three emit-pck JSON artifacts (pack.{manifest.json,contents.jsonl,
#     data.json}) — a fresh `emit-pck --pack chibi` must match byte-for-byte, and
#   * the body model's source bytes: the .glb the assembler runtime-loads plus its
#     skin dye-mask and neutral recolor-base PNGs the colour-race body-material
#     path samples (design 2026-07-14-chibi §6/R2; AssembledCharacter
#     ._load_model_scene / _load_mask_texture fall back to the staged source
#     sibling because the M0 pack is declarative).
# Chibi has NO golden corpus (the golden is the core determinism snapshot); this
# is the staged-pack half only. --update-golden refreshes it in lockstep.
CHIBI_PACK_NS="chibi"
CHIBI_STAGED_DIR="client/project/meridian/chibi"
CHIBI_STAGED_FILES=(pack.manifest.json pack.contents.jsonl pack.data.json)
# The chibi body sources. Entry format matches STAGED_ART:
# "<content-tree source>:<staged copy under $CHIBI_STAGED_DIR>". The recolor-base
# source is *_recolor_bc.png but stages at the assembler-resolved
# *_recolor_base.png (the recolor asset id's basename + .png).
CHIBI_STAGED_ART=(
  "content/chibi/assets/art/chibi_pill_body/sk_chibi_pill_body.glb:art/chibi_pill_body.glb"
  "content/chibi/assets/art/chibi_pill_body/chibi_pill_body_mask.png:art/chibi_pill_body_mask.png"
  "content/chibi/assets/art/chibi_pill_body/chibi_pill_body_recolor_bc.png:art/chibi_pill_body_recolor_base.png"
)

# --- The CHIBI staged CHUNK pack (story #877, epic #872). --------------------
# Sprout Meadow's terrain: the first zone to stream real chunks instead of the M0
# flat bootstrap. `mcc chunk-emit --profile meadow` is the GENERATOR (the bytes are
# generated from source on demand, never hand-authored), and the emitted pack is
# staged at res://meridian/chibi/chunks/sprout_meadow so the client mounts it.
#
# ⛔ WHY ONLY *SOME* OF chunk-emit's OUTPUT IS STAGED (the #877 pack reconciliation)
#
# `chunk-emit --out` writes, alongside the chunk payloads, its own
# `pack.manifest.json` + `pack.contents.jsonl` (a `meridian/pack-manifest@1` doc it
# renders for the standalone Story-0 fixture). Those two files are DELIBERATELY NOT
# STAGED, because `emit-pck --pack chibi` already writes the authoritative
# `meridian/pack-manifest@1` for this namespace at the mount ROOT
# ($CHIBI_STAGED_DIR/pack.manifest.json, gated in §7). Staging both would put two
# manifests for one namespace into the same mount, each listing a DISJOINT entry set
# with a different content_hash — the "competing pack manifest" the #872 scout
# flagged. Nothing would even read the second one:
#
#   * the client's zone mount (MeridianPackMount.verify_chunk_index, world.gd
#     _resolve_zone_paths) reads ONLY `<zone>.chunks.json` + `<zone>.assets.json`
#     and resolves payloads by the res:// paths in the asset table;
#   * the pack mount (pack_manifest_core) reads `<pack dir>/pack.manifest.json` —
#     i.e. emit-pck's, at the root, not one nested under chunks/<zone>/;
#   * the checked-in Story-0 chunkpack fixture likewise ships no pack.manifest.json.
#
# So the reconciliation is a SCOPE split, not a merge: emit-pck owns the pack
# manifest, chunk-emit owns the chunk payloads + the IF-6/IF-8 zone index. The two
# never write the same path. (Folding the chunk entries INTO emit-pck's manifest is
# a real option later — it would let the pack's content_hash cover terrain too — but
# it needs the IF-5 content-hash tie at enter-world, which is not wired at M0, and
# it is not required for the client to mount and stream. Out of scope for #877.)
CHIBI_CHUNK_ZONE="chibi:zone.sprout_meadow"
CHIBI_CHUNK_ZONE_BARE="sprout_meadow"
CHIBI_CHUNK_PROFILE="meadow"          # the #876 gentle-rolling profile (+-3 m R5 budget)
CHIBI_CHUNK_GRID=3                    # 3x3 @ 128 m ...
CHIBI_CHUNK_ORIGIN="-384"             # ... at zone01's geometry => zone-local [-512,-128]
CHIBI_CHUNK_STAGED_DIR="client/project/meridian/chibi/chunks/sprout_meadow"

# Emit Sprout Meadow's chunks into $1/, flattened to exactly the file set the client
# mount consumes (see the reconciliation note above). chunk-emit takes no --built-at:
# its output is a pure function of the geometry + profile, so it is reproducible with
# no timestamp to pin.
emit_chibi_chunks_into() {  # $1 = target dir
  local dir="$1"
  local tmp="$dir/.emit"
  mkdir -p "$dir"
  "$REPO_ROOT/$MCC" chunk-emit --zone "$CHIBI_CHUNK_ZONE" --profile "$CHIBI_CHUNK_PROFILE" \
      --grid "$CHIBI_CHUNK_GRID" --origin-x "$CHIBI_CHUNK_ORIGIN" \
      --origin-z "$CHIBI_CHUNK_ORIGIN" --out "$tmp" >/dev/null
  local src="$tmp/meridian/${CHIBI_PACK_NS}/chunks/${CHIBI_CHUNK_ZONE_BARE}"
  [ -d "$src" ] || die "chunk-emit wrote no chunk dir at $src"
  # The IF-6 manifest + IF-8 asset table + every chunk payload (.chunk.bin server
  # heightfield, .tscn scene, .proxy.tscn LOD). NOT pack.manifest.json /
  # pack.contents.jsonl — see above.
  cp "$src/${CHIBI_CHUNK_ZONE_BARE}.chunks.json" "$dir/"
  cp "$src/${CHIBI_CHUNK_ZONE_BARE}.assets.json" "$dir/"
  cp "$src"/*.chunk.bin "$src"/*.tscn "$dir/"
  rm -rf "$tmp"
}

# Emit ONLY the chibi client pack (emit-pck --pack chibi) into $1/, flattened to
# the same flat shape as the staged files. No world.sql/index — this gate polices
# the staged CLIENT mount only. Same fixed built_at as the golden so the
# pack.manifest.json built_at is reproducible.
emit_chibi_pck_into() {  # $1 = target dir
  local dir="$1"
  mkdir -p "$dir"
  "$REPO_ROOT/$MCC" emit-pck "$REPO_ROOT/$CONTENT_DIR" --pack "$CHIBI_PACK_NS" \
      --out "$dir/pck" --built-at "$GOLDEN_BUILT_AT" >/dev/null
  cp "$dir/pck/meridian/${CHIBI_PACK_NS}/pack.manifest.json"  "$dir/pack.manifest.json"
  cp "$dir/pck/meridian/${CHIBI_PACK_NS}/pack.contents.jsonl" "$dir/pack.contents.jsonl"
  cp "$dir/pck/meridian/${CHIBI_PACK_NS}/pack.data.json"      "$dir/pack.data.json"
  rm -rf "$dir/pck"
}

# --- 2a. --update-golden: regenerate the checked-in golden and stop. ---------
if [ "$UPDATE_GOLDEN" -eq 1 ]; then
  log "Regenerating golden corpus → $GOLDEN_DIR (built_at='$GOLDEN_BUILT_AT')"
  rm -rf "$GOLDEN_DIR"
  emit_into "$GOLDEN_DIR"
  ok "Golden corpus written:"
  for f in "${GOLDEN_FILES[@]}"; do
    printf '      %s  (%s bytes)\n' "$GOLDEN_DIR/$f" "$(wc -c < "$GOLDEN_DIR/$f" | tr -d ' ')"
  done
  # Keep the staged client pack in lockstep (the golden/staged pair is atomic —
  # the staleness gate below enforces it on every run).
  mkdir -p "$STAGED_DIR"
  for f in "${STAGED_FILES[@]}"; do
    cp "$GOLDEN_DIR/$f" "$STAGED_DIR/$f"
  done
  for entry in "${STAGED_ART[@]}"; do
    src="${entry%%:*}"; dst="$STAGED_DIR/${entry##*:}"
    [ -f "$src" ] || die "staged model source missing: $src"
    mkdir -p "$(dirname "$dst")"
    cp "$src" "$dst"
  done
  ok "Staged client pack refreshed: $STAGED_DIR/{${STAGED_FILES[*]// /,}} + ${#STAGED_ART[@]} model source(s)"
  # The CHIBI staged pack (story #809) — refreshed in the same lockstep. No golden
  # corpus for chibi; emit its pck fresh and stage the JSON + body source bytes.
  CHIBI_TMP="$(mktemp -d)"
  emit_chibi_pck_into "$CHIBI_TMP"
  mkdir -p "$CHIBI_STAGED_DIR"
  for f in "${CHIBI_STAGED_FILES[@]}"; do
    cp "$CHIBI_TMP/$f" "$CHIBI_STAGED_DIR/$f"
  done
  for entry in "${CHIBI_STAGED_ART[@]}"; do
    src="${entry%%:*}"; dst="$CHIBI_STAGED_DIR/${entry##*:}"
    [ -f "$src" ] || die "chibi staged model source missing: $src"
    mkdir -p "$(dirname "$dst")"
    cp "$src" "$dst"
  done
  rm -rf "$CHIBI_TMP"
  ok "Chibi staged client pack refreshed: $CHIBI_STAGED_DIR/{${CHIBI_STAGED_FILES[*]// /,}} + ${#CHIBI_STAGED_ART[@]} model source(s)"
  # Sprout Meadow's staged CHUNK pack (#877) — regenerated in the same lockstep.
  # Wiped first so a chunk dropped from the emit cannot linger as a staged orphan.
  rm -rf "$CHIBI_CHUNK_STAGED_DIR"
  emit_chibi_chunks_into "$CHIBI_CHUNK_STAGED_DIR"
  ok "Chibi staged chunk pack refreshed: $CHIBI_CHUNK_STAGED_DIR ($(find "$CHIBI_CHUNK_STAGED_DIR" -type f | wc -l | tr -d ' ') files)"
  warn "Review the golden diff like a content diff, then commit tools/mcc/golden/, $STAGED_DIR/, $CHIBI_STAGED_DIR/ AND $CHIBI_CHUNK_STAGED_DIR/ together."
  exit 0
fi

# --- 2b. The gate: golden must exist. ----------------------------------------
for f in "${GOLDEN_FILES[@]}"; do
  [ -f "$GOLDEN_DIR/$f" ] || die "golden file missing: $GOLDEN_DIR/$f — run 'scripts/check-golden.sh --update-golden' to create it"
done

# --- 3. CROSS-RUN determinism: build twice, compare byte-for-byte. -----------
log "Cross-run determinism: emitting content twice and comparing byte-for-byte"
RUN1="$(mktemp -d)"; RUN2="$(mktemp -d)"
trap 'rm -rf "$RUN1" "$RUN2"' EXIT
emit_into "$RUN1"
emit_into "$RUN2"
for f in "${GOLDEN_FILES[@]}"; do
  if ! cmp -s "$RUN1/$f" "$RUN2/$f"; then
    warn "cross-run diff for $f:"
    diff "$RUN1/$f" "$RUN2/$f" | head -40 >&2 || true
    die "NON-DETERMINISTIC: $f differs between two consecutive mcc builds (a P0 tools bug — same source + same mcc MUST be byte-identical)"
  fi
done
ok "Deterministic: two consecutive builds produced byte-identical output"

# --- 4. GOLDEN MATCH: fresh build (RUN1) vs the checked-in golden. -----------
log "Golden match: comparing fresh mcc output against $GOLDEN_DIR"
drift=0
for f in "${GOLDEN_FILES[@]}"; do
  if ! cmp -s "$RUN1/$f" "$GOLDEN_DIR/$f"; then
    drift=1
    warn "DRIFT in $f — golden vs fresh build:"
    diff "$GOLDEN_DIR/$f" "$RUN1/$f" | head -40 >&2 || true
  fi
done
if [ "$drift" -ne 0 ]; then
  die "mcc output drifted from golden — if this is an INTENDED content/output change run 'scripts/check-golden.sh --update-golden' and commit tools/mcc/golden/; otherwise you have introduced nondeterminism or an unintended change."
fi
ok "Golden match: fresh mcc output is byte-identical to the checked-in golden"

# --- 5. STAGED CLIENT PACK: the committed res://meridian/core copy matches. ---
# Guards the #477 hand-staged pack against silent drift: a content change that
# regenerates the golden but forgets the client stage would ship stale
# catalog/worn/dye data while every other gate stays green.
log "Staged client pack: comparing $STAGED_DIR against the fresh emit"
stale=0
for f in "${STAGED_FILES[@]}"; do
  if ! cmp -s "$RUN1/$f" "$STAGED_DIR/$f"; then
    stale=1
    warn "STALE staged pack file $STAGED_DIR/$f vs fresh emit:"
    diff "$STAGED_DIR/$f" "$RUN1/$f" 2>/dev/null | head -20 >&2 || true
  fi
done
for entry in "${STAGED_ART[@]}"; do
  src="${entry%%:*}"; dst="$STAGED_DIR/${entry##*:}"
  if ! cmp -s "$src" "$dst"; then
    stale=1
    warn "STALE staged model source $dst vs $src"
  fi
done
if [ "$stale" -ne 0 ]; then
  die "staged client pack is STALE — regenerate it with 'scripts/check-golden.sh --update-golden' and commit $STAGED_DIR/ together with tools/mcc/golden/."
fi
ok "Staged client pack matches the fresh emit (+ staged model sources match their content-tree sources)"

# --- 6. STAGED-MODEL COVERAGE: every committed model a renderable doc references
# has staged bytes. The STAGED_ART drift check above only guards the entries it
# LISTS; this independent gate catches the class of bug where a model's .glb is
# committed to content/ but the STAGED_ART entry was forgotten — the client would
# _fail to load it (story #569, the invisible-Warden's-Kit regression) while every
# other gate stayed green. Placeholders (no committed source bytes) are skipped.
log "Staged-model coverage: every committed referenced model has staged bytes"
if command -v python3 >/dev/null 2>&1; then
  python3 "$REPO_ROOT/tools/check_staged_models.py" \
      --content "$REPO_ROOT/$CONTENT_DIR" --staged "$REPO_ROOT/$STAGED_DIR" \
    || die "staged-model coverage FAILED — add the missing model(s) to STAGED_ART in $(basename "${BASH_SOURCE[0]}") and re-run --update-golden."
else
  # A missing python3 means the coverage gate cannot run — fail closed (die), not
  # open (warn), so a determinism run can never silently skip it (issue #583).
  die "python3 not found — cannot run the staged-model coverage gate (tools/check_staged_models.py); install python3 and re-run."
fi

# --- 7. CHIBI STAGED CLIENT PACK (story #809): the committed res://meridian/chibi
# copy matches a fresh `emit-pck --pack chibi`, and the chibi body source bytes are
# staged. Same guarantee as the core staged pack (§5/§6) for the second theme mount
# the dev realm loads (C9/#762): a clean checkout can never ship a stale/pre-roster
# chibi pack or an unrenderable chibi body. Chibi has no golden corpus, so emit a
# fresh single-pack chibi pck here and diff the committed mount against it.
log "Chibi staged client pack: comparing $CHIBI_STAGED_DIR against a fresh emit-pck --pack $CHIBI_PACK_NS"
CHIBI_RUN="$(mktemp -d)"
emit_chibi_pck_into "$CHIBI_RUN"
chibi_stale=0
for f in "${CHIBI_STAGED_FILES[@]}"; do
  if ! cmp -s "$CHIBI_RUN/$f" "$CHIBI_STAGED_DIR/$f"; then
    chibi_stale=1
    warn "STALE chibi staged pack file $CHIBI_STAGED_DIR/$f vs fresh emit:"
    diff "$CHIBI_STAGED_DIR/$f" "$CHIBI_RUN/$f" 2>/dev/null | head -20 >&2 || true
  fi
done
rm -rf "$CHIBI_RUN"
for entry in "${CHIBI_STAGED_ART[@]}"; do
  src="${entry%%:*}"; dst="$CHIBI_STAGED_DIR/${entry##*:}"
  if ! cmp -s "$src" "$dst"; then
    chibi_stale=1
    warn "STALE chibi staged model source $dst vs $src"
  fi
done
if [ "$chibi_stale" -ne 0 ]; then
  die "chibi staged client pack is STALE — regenerate it with 'scripts/check-golden.sh --update-golden' and commit $CHIBI_STAGED_DIR/."
fi
ok "Chibi staged client pack matches the fresh emit (+ staged body sources match their content-tree sources)"

# Staged-model coverage for the chibi mount too (check_staged_models is namespace-
# aware: --staged's dir name scopes it to chibi:* refs), so a future forgotten
# chibi body/hair source is caught exactly as §6 catches a core one.
log "Chibi staged-model coverage: every committed chibi model has staged bytes"
if command -v python3 >/dev/null 2>&1; then
  python3 "$REPO_ROOT/tools/check_staged_models.py" \
      --content "$REPO_ROOT/$CONTENT_DIR" --staged "$REPO_ROOT/$CHIBI_STAGED_DIR" \
    || die "chibi staged-model coverage FAILED — add the missing model(s) to CHIBI_STAGED_ART in $(basename "${BASH_SOURCE[0]}") and re-run --update-golden."
else
  die "python3 not found — cannot run the chibi staged-model coverage gate (tools/check_staged_models.py); install python3 and re-run."
fi

# --- 8. CHIBI STAGED CHUNK PACK (story #877): the committed Sprout Meadow terrain
# under res://meridian/chibi/chunks/sprout_meadow matches a fresh
# `chunk-emit --profile meadow`. This is what keeps the FIRST zone with real terrain
# honest: the staged chunks are generated bytes, so a profile/stride/material change
# that forgets the stage would ship terrain that no longer matches its generator —
# and the per-chunk BLAKE3 in the IF-6 manifest would stop matching the payloads,
# which fails the client's fail-closed verify AT ENTER-WORLD (a black-screen bug
# every other gate would wave through). Byte-for-byte, in BOTH directions, so a
# stale orphan left behind by a shrinking grid is caught too.
log "Chibi staged chunk pack: comparing $CHIBI_CHUNK_STAGED_DIR against a fresh chunk-emit --profile $CHIBI_CHUNK_PROFILE"
[ -d "$CHIBI_CHUNK_STAGED_DIR" ] || die "chibi staged chunk pack missing: $CHIBI_CHUNK_STAGED_DIR — run 'scripts/check-golden.sh --update-golden' to create it"
CHIBI_CHUNK_RUN="$(mktemp -d)"
emit_chibi_chunks_into "$CHIBI_CHUNK_RUN"
chunk_stale=0
# (a) every freshly emitted file is staged, byte-identical.
while IFS= read -r f; do
  rel="${f#"$CHIBI_CHUNK_RUN"/}"
  if [ ! -f "$CHIBI_CHUNK_STAGED_DIR/$rel" ]; then
    chunk_stale=1
    warn "MISSING staged chunk file $CHIBI_CHUNK_STAGED_DIR/$rel (the fresh emit produced it)"
  elif ! cmp -s "$f" "$CHIBI_CHUNK_STAGED_DIR/$rel"; then
    chunk_stale=1
    warn "STALE staged chunk file $CHIBI_CHUNK_STAGED_DIR/$rel vs fresh emit"
  fi
done < <(find "$CHIBI_CHUNK_RUN" -type f)
# (b) nothing EXTRA is staged (an orphan the generator no longer emits).
while IFS= read -r f; do
  rel="${f#"$CHIBI_CHUNK_STAGED_DIR"/}"
  if [ ! -f "$CHIBI_CHUNK_RUN/$rel" ]; then
    chunk_stale=1
    warn "ORPHAN staged chunk file $CHIBI_CHUNK_STAGED_DIR/$rel (the fresh emit does NOT produce it)"
  fi
done < <(find "$CHIBI_CHUNK_STAGED_DIR" -type f)
rm -rf "$CHIBI_CHUNK_RUN"
if [ "$chunk_stale" -ne 0 ]; then
  die "chibi staged chunk pack is STALE — regenerate it with 'scripts/check-golden.sh --update-golden' and commit $CHIBI_CHUNK_STAGED_DIR/."
fi
ok "Chibi staged chunk pack matches a fresh chunk-emit (Sprout Meadow terrain is current)"

log "${_B}Determinism gate passed.${_R} Golden corpus, staged client pack (core + chibi), Sprout Meadow chunks, and mcc output are all current and deterministic."
