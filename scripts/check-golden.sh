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
  "$REPO_ROOT/$MCC" emit-sql "$REPO_ROOT/$CONTENT_DIR" \
      --out "$dir/world.sql" --built-at "$GOLDEN_BUILT_AT" >/dev/null
  "$REPO_ROOT/$MCC" emit-pck "$REPO_ROOT/$CONTENT_DIR" \
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
  warn "Review the golden diff like a content diff, then commit tools/mcc/golden/ AND $STAGED_DIR/ together."
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

log "${_B}Determinism gate passed.${_R} Golden corpus, staged client pack, and mcc output are all current and deterministic."
