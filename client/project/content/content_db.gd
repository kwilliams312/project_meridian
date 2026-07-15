# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — MeridianContentDB: the client's read side of mcc-built pack
# content (issue #477, client-assembler spec ② §3). Autoloaded as `ContentDB`.
#
# WHAT IT IS. The character assembler (spec ②) and the char-create pickers need
# VISUAL data the wire never carries — only ids travel (item_template, dye_id,
# preset ids, race/sex); every lookup is client-side against the mounted pack.
# This singleton is that lookup. It reads the mcc emit-pck artifacts staged under
# a pack directory (default `res://meridian/core`, the res:// layout MeridianPackMount
# mounts — client SAD §2.3):
#   * pack.contents.jsonl — the M0 directory manifest: one JSON object per line,
#     {id, numeric_id, resource, hash}. Source of `model_path()` (id → res:// path).
#   * pack.data.json      — the M0 client-render field data (emit_pck.cpp, #477):
#     appearance catalogs, item `visual.worn`, and dye colors, keyed by IF-9
#     numeric id. The manifest/contents carry only id→resource→hash; the FIELDS
#     ride this dep-free JSON sidecar until the Godot-native .pck import lands.
#
# ⚠ M0 boundary (honest, per emit_pck.h): the pck's per-type `tables/<type>.bin`
# are DECLARATIVE res:// paths — no Godot importer runs at M0, so the visual field
# values ship in pack.data.json, not in a FlatBuffers blob. When the real .pck
# mount lands this class swaps its source without changing the API below.
#
# ERROR DISCIPLINE (spec §6, contract ① §9): every miss returns a documented empty
# sentinel — never a crash, never a fabricated value. A pack that will not load
# leaves the DB empty; callers (the assembler, the pickers) degrade to their
# fallback (capsule / "content missing") off `is_loaded()` + the empty returns.
#
# PRODUCED API (BINDING for the assembler stories — spec ② Tasks 3/4):
#   catalog(race, sex)  -> {} if missing; else {body_model, skeleton, presets:{hair,face,skin}}
#   worn(item_template) -> {} if none;    else {models, hides, attach, dye_channels, race_overrides}
#   dye_color(dye_id)   -> Color(0,0,0,0) sentinel if unknown
#   model_path(id)      -> "" if unknown (accepts a String content id OR an int numeric id)
#
# ACCESS PATTERN. In the running app this script is the `ContentDB` autoload
# (project.godot). But the repo's headless verify harness runs scripts STANDALONE
# (`godot --headless --path <proj> --script <p>`), where Godot does NOT initialize
# autoloads — a bare `ContentDB` identifier is a compile error there. So consumer
# scripts reference the CLASS (resolved via the global class cache, which standalone
# runs DO have — same as MeridianRoster) through `MeridianContentDB.instance()`:
# the autoload when the app is running (it registers itself in _init), else a
# lazily-created shared instance (verify runs). Loading happens in _init — no scene
# tree required — so both paths see identical content.

extends Node
class_name MeridianContentDB

## The env var a dev/CI run can point at a freshly built pack (e.g.
## build/content-out/pck/meridian/core) instead of the staged res:// copy. This is
## an EXPLICIT full pack directory and wins over the theme selection below.
const PACK_DIR_ENV: String = "MERIDIAN_PACK_DIR"

## The realm's selected content THEME — the pack namespace whose staged mount the
## client loads (issue #791; design §4/§8). Mirrors worldd's MERIDIAN_REALM_THEME
## (server/worldd/world_boot: the realm's primary pack_namespace) so the client
## mounts the SAME pack the connected realm serves — a chibi realm (theme=chibi)
## shows chibi. Default "core" keeps the pre-#791 behaviour (back-compat). The
## handshake also advertises the realm's content_hash (IF-2 HandshakeOk) for a
## later fail-closed content-hash tie at enter-world (MeridianPackMount
## .set_expected_content_hash); until that path is wired, this theme selector is the
## local-dev bridge that picks WHICH res://meridian/<ns> to mount.
const REALM_THEME_ENV: String = "MERIDIAN_REALM_THEME"

## The res:// root every staged theme pack lives under: res://meridian/<theme>
## (e.g. res://meridian/core, res://meridian/chibi). MeridianPackMount mounts this
## same res:// layout (client SAD §2.3).
const PACK_MOUNT_ROOT: String = "res://meridian"

## The baseline theme when MERIDIAN_REALM_THEME is unset (matches worldd's
## kDefaultRealmTheme). "core" is the IF-9 baseline namespace.
const DEFAULT_THEME: String = "core"

## The staged pack the client ships when no override/theme is set (the res://
## meridian/core mount). Kept as a named constant for callers/tests that reference
## the default explicitly; derived from the root + default theme so the two never
## drift.
const DEFAULT_PACK_DIR: String = PACK_MOUNT_ROOT + "/" + DEFAULT_THEME

## The authored-default sentinel a dye miss returns (contract ① §9: "unknown dye →
## authored colors"). Fully transparent so a caller can tell a real color from a
## miss and fall through to the item's authored surface colors.
const UNKNOWN_DYE: Color = Color(0, 0, 0, 0)

# The process-wide shared instance `instance()` serves: the autoload in the app
# (registered in _init), else the first lazily-created one (standalone verifies).
# Untyped on purpose — GDScript self-referential static typing is avoided for
# maximum parse-safety; instance() callers use explicit-typed locals.
static var _instance = null

var _loaded: bool = false
var _pack_dir: String = ""

# id → res:// resource path (from pack.contents.jsonl), and the numeric mirror.
var _res_by_id: Dictionary = {}
var _res_by_numeric: Dictionary = {}
var _numeric_by_id: Dictionary = {}

# The field data (from pack.data.json), keyed for the wire lookups.
var _catalog_by_race_sex: Dictionary = {}   # "ardent|male" -> {body_model, skeleton, presets}
var _worn_by_numeric: Dictionary = {}       # item_template numeric -> worn dict
var _dye_by_numeric: Dictionary = {}        # dye numeric -> Color


## The process-wide MeridianContentDB: the `ContentDB` autoload when the app is
## running, else a lazily-created shared instance (standalone --script verify runs,
## where autoloads never initialize). Untyped return so calls through it bind
## dynamically — callers assign to explicit-typed locals.
static func instance():
	if _instance == null:
		_instance = new()
	return _instance


## Resolve the pack directory the client mounts, applying the #791 precedence:
##   1. MERIDIAN_PACK_DIR — an EXPLICIT full pack dir (dev/CI fresh-build override).
##   2. MERIDIAN_REALM_THEME — the realm theme namespace → res://meridian/<theme>
##      (mirrors worldd's primary pack_namespace, so the client mounts the pack the
##      connected realm serves; the theme is sanitised to a single path segment).
##   3. Default → res://meridian/core (unchanged pre-#791 behaviour, back-compat).
## Static so the boot flow, tests, and any future handshake-driven resolver share
## one source of truth for "which staged pack does this client mount".
static func resolve_pack_dir() -> String:
	var override_dir := OS.get_environment(PACK_DIR_ENV)
	if not override_dir.is_empty():
		return override_dir
	return PACK_MOUNT_ROOT.path_join(resolve_theme())


## The realm theme namespace this client mounts: MERIDIAN_REALM_THEME when set to a
## safe single segment, else DEFAULT_THEME ("core"). A theme carrying a path
## separator, "..", or other unsafe characters is REJECTED (falls back to core with
## a warning) so an env value can never escape res://meridian/ into an arbitrary
## mount. The theme is a pack namespace — the same [a-z0-9_] shape mcc allocates.
static func resolve_theme() -> String:
	var theme := OS.get_environment(REALM_THEME_ENV).strip_edges()
	if theme.is_empty():
		return DEFAULT_THEME
	if not _is_safe_theme(theme):
		push_warning("MeridianContentDB: ignoring unsafe MERIDIAN_REALM_THEME '%s' — falling back to '%s'." % [theme, DEFAULT_THEME])
		return DEFAULT_THEME
	return theme


## A theme namespace is a single lowercase [a-z0-9_] path segment (mcc's namespace
## shape). Rejects empty, path separators, "..", and anything that could traverse
## out of res://meridian/.
static func _is_safe_theme(theme: String) -> bool:
	if theme.is_empty():
		return false
	for c in theme:
		var ok := (c >= "a" and c <= "z") or (c >= "0" and c <= "9") or c == "_"
		if not ok:
			return false
	return true


func _init() -> void:
	# First instance wins the singleton slot (the autoload in the app — it is
	# constructed at boot before any consumer; the lazily-created one in verifies).
	if _instance == null:
		_instance = self
	# Resolve the pack directory (issue #791): an explicit MERIDIAN_PACK_DIR wins
	# (dev/CI pointing at a fresh build), else the realm THEME selects
	# res://meridian/<theme> (MERIDIAN_REALM_THEME, default "core" — mirrors worldd
	# so the client mounts the pack the connected realm serves). Loading needs no
	# scene tree, so it runs here (not _ready) and works identically for the autoload
	# and standalone instances. A load failure is NOT fatal — the DB stays empty and
	# callers use their content-missing fallback (spec §6).
	var dir := resolve_pack_dir()
	if not load_from(dir):
		push_warning("MeridianContentDB: no pack loaded from '%s' — content-dependent UI degrades to its fallback (spec ②/§6)." % dir)


## Load (or reload) the pack under `pack_dir`. Returns true iff at least the field
## data OR the contents manifest was read. Idempotent: clears prior state first, so
## a dev can point it at a new build and re-load. Safe to call from tests.
func load_from(pack_dir: String) -> bool:
	_clear()
	_pack_dir = pack_dir
	var got_contents := _load_contents(pack_dir.path_join("pack.contents.jsonl"))
	var got_data := _load_data(pack_dir.path_join("pack.data.json"))
	_loaded = got_contents or got_data
	return _loaded


## True once a pack has been loaded (at least one artifact read). Callers gate their
## content-missing fallback on this + the empty returns.
func is_loaded() -> bool:
	return _loaded


## The directory the current content was loaded from ("" if none) — for diagnostics.
func pack_dir() -> String:
	return _pack_dir


# --- The BINDING lookup API (spec ② Tasks 3/4) --------------------------------

## The appearance catalog for a race/sex, or {} when none is mounted (→ capsule
## fallback + "content missing" pickers, spec §6). `race` is the MeridianRoster id
## (1 = Ardent); `sex` is 0 = male, 1 = female (M1 ships male only). Returned dict:
## {body_model, skeleton, presets:{hair:[{id,model}], face:[{id,model}], skin:[{id,model}]}}.
func catalog(race: int, sex: int) -> Dictionary:
	return _catalog_by_race_sex.get(_race_sex_key(race, sex), {})


## The `visual.worn` block for an item template (the IF-9 numeric id the wire
## carries), or {} when the item has no worn data / is unknown (→ hide the piece,
## spec §6). Returned dict: {models:[{model,mirror}], hides:[], attach:{socket
## [, sheath_socket — omitted when unauthored]}, dye_channels:[],
## race_overrides:{<race>:{models:[{model,mirror}], hides:[]}}} — overrides carry
## the FULL schema shape; the assembler substitutes them wholesale (spec ② §4).
func worn(item_template: int) -> Dictionary:
	return _worn_by_numeric.get(item_template, {})


## The authored color for a dye id (the IF-9 numeric id the wire carries), or the
## UNKNOWN_DYE sentinel Color(0,0,0,0) when unknown (→ item's authored colors, §6).
func dye_color(dye_id: int) -> Color:
	return _dye_by_numeric.get(dye_id, UNKNOWN_DYE)


## The res:// resource path for an asset/entity, or "" when unknown. Accepts EITHER
## a fully-qualified String content id ("core:art.char.ardent.male.base") — the form
## catalog/worn field values use — OR an int IF-9 numeric id. The assembler resolves
## catalog `body_model` / worn `models[].model` (String ids) through here.
func model_path(id) -> String:
	if id is int:
		return _res_by_numeric.get(id, "")
	if id is String:
		return _res_by_id.get(id, "")
	return ""


## The IF-9 numeric id for a content id, or 0 when unknown. Helper for callers that
## hold a content id but need the wire/numeric key (e.g. tests, dye/worn resolution).
func numeric_id_for(content_id: String) -> int:
	return _numeric_by_id.get(content_id, 0)


# --- Parsing ------------------------------------------------------------------

func _clear() -> void:
	_loaded = false
	_pack_dir = ""
	_res_by_id.clear()
	_res_by_numeric.clear()
	_numeric_by_id.clear()
	_catalog_by_race_sex.clear()
	_worn_by_numeric.clear()
	_dye_by_numeric.clear()


# pack.contents.jsonl — one JSON object per line: {id, numeric_id, resource, hash}.
# Builds the id↔numeric↔resource maps model_path()/numeric_id_for() serve.
func _load_contents(path: String) -> bool:
	if not FileAccess.file_exists(path):
		return false
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		return false
	while not f.eof_reached():
		var line := f.get_line().strip_edges()
		if line.is_empty():
			continue
		var row = JSON.parse_string(line)
		if typeof(row) != TYPE_DICTIONARY:
			continue
		var id := String(row.get("id", ""))
		var numeric := int(row.get("numeric_id", 0))
		var resource := String(row.get("resource", ""))
		if id.is_empty():
			continue
		_res_by_id[id] = resource
		_numeric_by_id[id] = numeric
		if numeric != 0:
			_res_by_numeric[numeric] = resource
	return true


# pack.data.json — {schema, namespace, appearance:[...], dye:[...], item:[...]}.
# Indexes each type for the binding lookups.
func _load_data(path: String) -> bool:
	if not FileAccess.file_exists(path):
		return false
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		return false
	var parsed = JSON.parse_string(f.get_as_text())
	if typeof(parsed) != TYPE_DICTIONARY:
		return false

	for entry in parsed.get("appearance", []):
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		var key := _race_sex_name_key(String(entry.get("race", "")), String(entry.get("sex", "")))
		_catalog_by_race_sex[key] = {
			"body_model": String(entry.get("body_model", "")),
			"skeleton": String(entry.get("skeleton", "")),
			"presets": entry.get("presets", {"hair": [], "face": [], "skin": []}),
		}

	for entry in parsed.get("item", []):
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		var numeric := int(entry.get("numeric_id", 0))
		if numeric != 0 and entry.has("worn"):
			_worn_by_numeric[numeric] = entry["worn"]

	for entry in parsed.get("dye", []):
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		var numeric := int(entry.get("numeric_id", 0))
		var hex := String(entry.get("color", ""))
		if numeric != 0 and hex.is_valid_html_color():
			_dye_by_numeric[numeric] = Color.html(hex)

	return true


# race id + sex id -> the "<race>|<sex>" key the catalog is stored under. Maps the
# wire ids to the catalog's names via MeridianRoster (the single race-id source).
func _race_sex_key(race: int, sex: int) -> String:
	return _race_sex_name_key(MeridianRoster.race_name(race).to_lower(), _sex_name(sex))


func _race_sex_name_key(race_name: String, sex_name: String) -> String:
	return "%s|%s" % [race_name, sex_name]


# M1: sex 0 = male, 1 = female (the wire reserves sex for the additive future; the
# character row has no sex column yet, so callers pass 0).
func _sex_name(sex: int) -> String:
	return "female" if sex == 1 else "male"
