// tools/mcc/src/stages/emit_sql.cpp — emit-sql stage (Tools SAD §2.6, IF-4).
//
// Walks the linked content model and emits the deterministic world DB DML (the
// content-table inserts + the world_manifest row). See emit_sql.h for the IF-4
// contract. The per-type mapping mirrors schema/sql/world/*.sql exactly (the DDL
// is the co-reviewed contract) — flattening nested objects to prefixed columns,
// intRange to _min/_max, arrays of objects to child tables keyed (parent, ordinal),
// oneOf variants to a discriminated child table, and every *Ref/asset ref to its
// IF-9 numeric id via the link stage's idmap.

#include "stages/emit_sql.h"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "hash/blake3.h"
#include "stages/content_hash.h"
#include "stages/idmap.h"

namespace mcc::stages {

namespace {

// ---- SQL scalar formatting -------------------------------------------------

// Escape a string for a single-quoted SQL literal (MariaDB): double the quote,
// backslash the backslash, and escape control chars that could break the dump.
std::string sql_quote(const std::string& s) {
    std::string out = "'";
    for (const char c : s) {
        switch (c) {
            case '\'': out += "''"; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\0': out += "\\0"; break;
            default: out += c; break;
        }
    }
    out += "'";
    return out;
}

// Format a float deterministically: fixed shortest round-trippable form. yaml-cpp
// gives us the source scalar text; for numbers we re-emit the parsed double with
// enough precision to round-trip, trimming trailing zeros so 8.0 -> "8", 1.15 ->
// "1.15" (stable across platforms — no locale, no exponent for our value ranges).
// Trim a trailing fractional zero tail from a fixed-notation decimal ("22.50" ->
// "22.5", "88.0" -> "88"); leaves integers and non-fractional strings untouched.
std::string trim_frac_zeros(std::string s) {
    if (s.find('.') == std::string::npos) return s;
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

std::string fmt_double(double v) {
    // Shortest FIXED-notation decimal that round-trips to the same double. We
    // force std::fixed (never exponent form — MariaDB FLOAT columns take both,
    // but fixed keeps the dump clean + diff-friendly, e.g. -320 not -3.2e+02) and
    // search precisions 0..17 for the first whose parse equals v. Deterministic;
    // classic locale. Our value ranges (zone-local coords, speeds, percentages)
    // are small enough that fixed notation is always compact.
    for (int prec = 0; prec <= 17; ++prec) {
        std::ostringstream ss;
        ss.imbue(std::locale::classic());
        ss << std::fixed;
        ss.precision(prec);
        ss << v;
        const std::string s = ss.str();
        double parsed = 0.0;
        std::istringstream is(s);
        is.imbue(std::locale::classic());
        is >> parsed;
        if (parsed == v) return trim_frac_zeros(s);
    }
    // Extremely large/small magnitudes: fall back to the full precision form.
    std::ostringstream ss;
    ss.imbue(std::locale::classic());
    ss << std::setprecision(17) << v;
    return ss.str();
}

// ---- Value column: a single INSERT cell (NULL / number / quoted text). ------
struct Val {
    enum Kind { Null, Raw, Text } kind = Null;
    std::string text;  // raw literal (Raw) or the unescaped string (Text)

    static Val null() { return Val{Null, ""}; }
    static Val num(std::int64_t n) { return Val{Raw, std::to_string(n)}; }
    static Val u(std::uint64_t n) { return Val{Raw, std::to_string(n)}; }
    static Val raw(std::string r) { return Val{Raw, std::move(r)}; }
    static Val str(std::string s) { return Val{Text, std::move(s)}; }
    static Val boolean(bool b) { return Val{Raw, b ? "TRUE" : "FALSE"}; }

    std::string render() const {
        switch (kind) {
            case Null: return "NULL";
            case Raw: return text;
            case Text: return sql_quote(text);
        }
        return "NULL";
    }
};

// A single row destined for a table: an ordered list of cells.
struct Row {
    std::uint64_t sort_key = 0;  // primary sort (numeric PK) for deterministic order
    std::string sort_key2;        // secondary sort (string) for composite keys
    std::vector<Val> cells;
};

// A table accumulator: name, column list, and rows (sorted before emission).
struct Table {
    std::string name;
    std::vector<std::string> columns;
    std::vector<Row> rows;
};

// ---- yaml-cpp node accessors (defensive; content is already validated) ------

bool has(const YAML::Node& n, const char* key) {
    return n.IsMap() && n[key] && !n[key].IsNull();
}

std::string as_str(const YAML::Node& n) { return n.Scalar(); }

std::int64_t as_int(const YAML::Node& n) {
    try {
        return n.as<std::int64_t>();
    } catch (...) {
        return 0;
    }
}

double as_double(const YAML::Node& n) {
    try {
        return n.as<double>();
    } catch (...) {
        return 0.0;
    }
}

bool as_bool(const YAML::Node& n) {
    try {
        return n.as<bool>();
    } catch (...) {
        const std::string s = n.Scalar();
        return s == "true" || s == "1" || s == "yes";
    }
}

// ---- Ref resolution: string content/asset id -> IF-9 numeric id -------------

// Resolves a *Ref/asset-ref string (bare or namespaced) to its numeric id using
// the per-pack idmaps. A ref with no leading "<ns>:" defaults to the owning
// entity's namespace (schema README §References; same rule as link.cpp).
class Resolver {
public:
    Resolver(const std::map<std::string, idmap::IdMap>& idmaps, diag::Diagnostics& diags)
        : diags_(diags) {
        for (const auto& [ns, m] : idmaps) {
            for (const auto& [id, idx] : m.map) {
                numeric_[id] = idmap::numeric_id(m.band, idx);
            }
        }
    }

    // Resolve `ref` (owned by an entity in `owner_ns`) to a numeric id. On a miss,
    // emits an E001 error against `file`/`where` and returns 0 (a NULL-safe
    // sentinel — 0 is the reserved null id, never a real entity).
    std::uint32_t resolve(const std::string& ref, const std::string& owner_ns,
                          const std::string& file, const std::string& where) {
        const std::string full =
            ref.find(':') != std::string::npos ? ref : owner_ns + ":" + ref;
        const auto it = numeric_.find(full);
        if (it == numeric_.end()) {
            diags_.error("E001", file, where,
                         "emit-sql: reference '" + full +
                             "' has no IF-9 numeric id (idmap.lock out of date?)");
            return 0;
        }
        return it->second;
    }

private:
    std::unordered_map<std::string, std::uint32_t> numeric_;
    diag::Diagnostics& diags_;
};

// The namespace segment of a fully-qualified id ("core:npc.x" -> "core").
std::string ns_of(const std::string& id) {
    const std::size_t colon = id.find(':');
    return colon == std::string::npos ? std::string() : id.substr(0, colon);
}

// ---- intRange helper: {min,max} -> (min, max), each optional ----------------
struct IntRange {
    bool present = false;
    std::int64_t min = 0, max = 0;
};
IntRange int_range(const YAML::Node& parent, const char* key) {
    IntRange r;
    if (!has(parent, key)) return r;
    const YAML::Node n = parent[key];
    if (has(n, "min")) r.min = as_int(n["min"]);
    if (has(n, "max")) r.max = as_int(n["max"]);
    r.present = true;
    return r;
}

// ============================================================================
// Per-type emitters. Each takes the entity's numeric id, its YAML root, the
// owning namespace, the file (for diagnostics), the Resolver, and the set of
// Tables to append rows to (looked up by name in the caller's table map).
// ============================================================================

// Convenience: append a row to a named table with a numeric sort key.
Row& new_row(Table& t, std::uint64_t sort_key) {
    t.rows.emplace_back();
    t.rows.back().sort_key = sort_key;
    return t.rows.back();
}

// The whole emit context: the table map + resolver, so emitters stay terse.
struct Ctx {
    std::map<std::string, Table>& tables;
    Resolver& resolver;

    Table& tbl(const std::string& name) { return tables.at(name); }
    std::uint32_t ref(const YAML::Node& n, const std::string& ns, const std::string& file,
                      const std::string& where) {
        return resolver.resolve(as_str(n), ns, file, where);
    }
    // A nullable numeric-ref cell: NULL when absent, else the resolved id.
    Val ref_cell(const YAML::Node& parent, const char* key, const std::string& ns,
                 const std::string& file) {
        if (!has(parent, key)) return Val::null();
        return Val::u(ref(parent[key], ns, file, std::string("$.") + key));
    }
};

void emit_npc(std::uint32_t id, const YAML::Node& n, const std::string& ns,
              const std::string& file, Ctx& cx) {
    Row& r = new_row(cx.tbl("npc_template"), id);
    const IntRange lvl = int_range(n, "level");
    const YAML::Node stats = n["stats"];
    const YAML::Node ai = has(n, "ai") ? n["ai"] : YAML::Node();
    const YAML::Node inter = has(n, "interaction") ? n["interaction"] : YAML::Node();
    const YAML::Node loot = has(n, "loot") ? n["loot"] : YAML::Node();
    const YAML::Node vis = has(n, "visual") ? n["visual"] : YAML::Node();
    const YAML::Node mv = has(n, "movement") ? n["movement"] : YAML::Node();
    const IntRange dmg = int_range(stats, "damage");
    const IntRange money = int_range(loot, "money");

    r.cells = {
        Val::u(id),
        Val::str(as_str(n["name"])),
        has(n, "subtitle") ? Val::str(as_str(n["subtitle"])) : Val::null(),
        Val::num(lvl.min), Val::num(lvl.max),
        Val::str(as_str(n["creature_type"])),
        Val::str(has(n, "rank") ? as_str(n["rank"]) : "normal"),
        Val::str(as_str(n["faction"])),
        // stats.*
        Val::num(as_int(stats["health"])),
        has(stats, "mana") ? Val::num(as_int(stats["mana"])) : Val::null(),
        has(stats, "armor") ? Val::num(as_int(stats["armor"])) : Val::null(),
        dmg.present ? Val::num(dmg.min) : Val::null(),
        dmg.present ? Val::num(dmg.max) : Val::null(),
        Val::num(as_int(stats["attack_speed_ms"])),
        // ai.*
        has(ai, "behavior") ? Val::str(as_str(ai["behavior"])) : Val::null(),
        has(ai, "aggro_radius_m") ? Val::raw(fmt_double(as_double(ai["aggro_radius_m"]))) : Val::null(),
        has(ai, "leash_radius_m") ? Val::raw(fmt_double(as_double(ai["leash_radius_m"]))) : Val::null(),
        has(ai, "call_for_help_radius_m") ? Val::raw(fmt_double(as_double(ai["call_for_help_radius_m"]))) : Val::null(),
        has(ai, "flee_at_health_pct") ? Val::raw(fmt_double(as_double(ai["flee_at_health_pct"]))) : Val::null(),
        // movement.*
        has(mv, "walk_speed_mps") ? Val::raw(fmt_double(as_double(mv["walk_speed_mps"]))) : Val::null(),
        has(mv, "run_speed_mps") ? Val::raw(fmt_double(as_double(mv["run_speed_mps"]))) : Val::null(),
        // interaction.vendor
        cx.ref_cell(inter, "vendor", ns, file),
        // loot.*
        cx.ref_cell(loot, "table", ns, file),
        money.present ? Val::u(static_cast<std::uint64_t>(money.min)) : Val::null(),
        money.present ? Val::u(static_cast<std::uint64_t>(money.max)) : Val::null(),
        // visual.*
        cx.ref_cell(vis, "model", ns, file),
        has(vis, "scale") ? Val::raw(fmt_double(as_double(vis["scale"]))) : Val::null(),
        cx.ref_cell(vis, "sound_set", ns, file),
    };

    // ai.abilities[] -> npc_ability
    if (has(ai, "abilities") && ai["abilities"].IsSequence()) {
        for (const auto& ab : ai["abilities"]) {
            const std::uint32_t ability_id = cx.ref(ab["ability"], ns, file, "$.ai.abilities[].ability");
            Row& ar = new_row(cx.tbl("npc_ability"), id);
            ar.sort_key2 = std::to_string(ability_id);
            ar.cells = {
                Val::u(id), Val::u(ability_id),
                has(ab, "priority") ? Val::num(as_int(ab["priority"])) : Val::num(0),
                has(ab, "cooldown_override_ms") ? Val::num(as_int(ab["cooldown_override_ms"])) : Val::null(),
                has(ab, "use_at_health_below_pct") ? Val::raw(fmt_double(as_double(ab["use_at_health_below_pct"]))) : Val::null(),
            };
        }
    }

    // interaction.gossip_text -> gossip (one page per npc)
    if (has(inter, "gossip_text")) {
        Row& gr = new_row(cx.tbl("gossip"), id);
        gr.cells = {Val::u(id), Val::str(as_str(inter["gossip_text"]))};
    }
}

void emit_item(std::uint32_t id, const YAML::Node& n, const std::string& ns,
               const std::string& file, Ctx& cx) {
    Row& r = new_row(cx.tbl("item_template"), id);
    const YAML::Node weapon = has(n, "weapon") ? n["weapon"] : YAML::Node();
    const YAML::Node price = has(n, "price") ? n["price"] : YAML::Node();
    const YAML::Node vis = has(n, "visual") ? n["visual"] : YAML::Node();
    const YAML::Node effects = has(n, "effects") ? n["effects"] : YAML::Node();
    const IntRange wdmg = int_range(weapon, "damage");

    r.cells = {
        Val::u(id),
        Val::str(as_str(n["name"])),
        has(n, "flavor_text") ? Val::str(as_str(n["flavor_text"])) : Val::null(),
        Val::str(as_str(n["item_class"])),
        has(n, "subclass") ? Val::str(as_str(n["subclass"])) : Val::null(),
        has(n, "slot") ? Val::str(as_str(n["slot"])) : Val::null(),
        Val::str(as_str(n["rarity"])),
        Val::num(has(n, "required_level") ? as_int(n["required_level"]) : 1),
        Val::num(has(n, "item_level") ? as_int(n["item_level"]) : 1),
        Val::boolean(has(n, "unique") ? as_bool(n["unique"]) : false),
        Val::str(has(n, "binding") ? as_str(n["binding"]) : "none"),
        Val::num(has(n, "stack_size") ? as_int(n["stack_size"]) : 1),
        // weapon.*
        wdmg.present ? Val::num(wdmg.min) : Val::null(),
        wdmg.present ? Val::num(wdmg.max) : Val::null(),
        has(weapon, "speed_ms") ? Val::num(as_int(weapon["speed_ms"])) : Val::null(),
        has(weapon, "school") ? Val::str(as_str(weapon["school"])) : Val::null(),
        has(n, "armor") ? Val::num(as_int(n["armor"])) : Val::null(),
        // effects.on_use
        cx.ref_cell(effects, "on_use", ns, file),
        // price.*
        has(price, "sell") ? Val::u(static_cast<std::uint64_t>(as_int(price["sell"]))) : Val::null(),
        has(price, "buy") ? Val::u(static_cast<std::uint64_t>(as_int(price["buy"]))) : Val::null(),
        // visual.*
        cx.ref_cell(vis, "icon", ns, file),
        cx.ref_cell(vis, "model", ns, file),
    };

    // stats[] -> item_stat
    if (has(n, "stats") && n["stats"].IsSequence()) {
        for (const auto& s : n["stats"]) {
            Row& sr = new_row(cx.tbl("item_stat"), id);
            sr.sort_key2 = as_str(s["stat"]);
            sr.cells = {Val::u(id), Val::str(as_str(s["stat"])), Val::num(as_int(s["amount"]))};
        }
    }

    // effects.on_equip[] -> item_effect_on_equip
    if (has(effects, "on_equip") && effects["on_equip"].IsSequence()) {
        std::uint16_t ord = 0;
        for (const auto& e : effects["on_equip"]) {
            const std::uint32_t ability_id = cx.ref(e, ns, file, "$.effects.on_equip[]");
            Row& er = new_row(cx.tbl("item_effect_on_equip"), id);
            er.sort_key2 = std::to_string(ord);
            er.cells = {Val::u(id), Val::num(ord), Val::u(ability_id)};
            ++ord;
        }
    }
}

void emit_ability(std::uint32_t id, const YAML::Node& n, const std::string& ns,
                  const std::string& file, Ctx& cx) {
    Row& r = new_row(cx.tbl("ability"), id);
    const YAML::Node cast = has(n, "cast") ? n["cast"] : YAML::Node();
    const YAML::Node res = has(n, "resource") ? n["resource"] : YAML::Node();
    const YAML::Node av = has(n, "audio_visual") ? n["audio_visual"] : YAML::Node();

    r.cells = {
        Val::u(id),
        Val::str(as_str(n["name"])),
        has(n, "description") ? Val::str(as_str(n["description"])) : Val::null(),
        Val::str(as_str(n["school"])),
        Val::str(as_str(n["target"])),
        Val::raw(fmt_double(has(n, "range_m") ? as_double(n["range_m"]) : 5.0)),
        has(cast, "time_ms") ? Val::num(as_int(cast["time_ms"])) : Val::num(0),
        has(cast, "channel_ms") ? Val::num(as_int(cast["channel_ms"])) : Val::null(),
        Val::num(has(n, "cooldown_ms") ? as_int(n["cooldown_ms"]) : 0),
        Val::boolean(has(n, "triggers_gcd") ? as_bool(n["triggers_gcd"]) : true),
        has(res, "type") ? Val::str(as_str(res["type"])) : Val::null(),
        has(res, "amount") ? Val::num(as_int(res["amount"])) : Val::null(),
        has(av, "cast_anim") ? Val::str(as_str(av["cast_anim"])) : Val::null(),
        cx.ref_cell(av, "cast_vfx", ns, file),
        cx.ref_cell(av, "cast_sfx", ns, file),
        cx.ref_cell(av, "impact_vfx", ns, file),
        cx.ref_cell(av, "impact_sfx", ns, file),
    };

    // effects[] -> ability_effect (oneOf by kind)
    if (has(n, "effects") && n["effects"].IsSequence()) {
        std::uint16_t ord = 0;
        for (const auto& e : n["effects"]) {
            const std::string kind = as_str(e["kind"]);
            const IntRange amount = int_range(e, "amount");
            const YAML::Node periodic = has(e, "periodic") ? e["periodic"] : YAML::Node();
            const IntRange pamount = int_range(periodic, "amount");
            Row& er = new_row(cx.tbl("ability_effect"), id);
            er.sort_key2 = std::to_string(ord);
            er.cells = {
                Val::u(id), Val::num(ord), Val::str(kind),
                amount.present ? Val::num(amount.min) : Val::null(),
                amount.present ? Val::num(amount.max) : Val::null(),
                has(e, "coefficient") ? Val::raw(fmt_double(as_double(e["coefficient"]))) : Val::null(),
                has(e, "threat_amount") ? Val::num(as_int(e["threat_amount"])) : Val::null(),
                has(e, "duration_ms") ? Val::num(as_int(e["duration_ms"])) : Val::null(),
                has(e, "max_stacks") ? Val::num(as_int(e["max_stacks"])) : Val::null(),
                has(periodic, "kind") ? Val::str(as_str(periodic["kind"])) : Val::null(),
                pamount.present ? Val::num(pamount.min) : Val::null(),
                pamount.present ? Val::num(pamount.max) : Val::null(),
                has(periodic, "tick_ms") ? Val::num(as_int(periodic["tick_ms"])) : Val::null(),
            };

            // aura.stat_mods[] -> ability_effect_stat_mod
            if (has(e, "stat_mods") && e["stat_mods"].IsSequence()) {
                for (const auto& sm : e["stat_mods"]) {
                    Row& smr = new_row(cx.tbl("ability_effect_stat_mod"), id);
                    smr.sort_key2 = std::to_string(ord) + "|" + as_str(sm["stat"]);
                    smr.cells = {Val::u(id), Val::num(ord), Val::str(as_str(sm["stat"])),
                                 Val::num(as_int(sm["amount"]))};
                }
            }
            ++ord;
        }
    }
}

void emit_quest(std::uint32_t id, const YAML::Node& n, const std::string& ns,
                const std::string& file, Ctx& cx) {
    Row& r = new_row(cx.tbl("quest_template"), id);
    const YAML::Node rewards = has(n, "rewards") ? n["rewards"] : YAML::Node();
    const std::uint32_t giver = cx.ref(n["giver"], ns, file, "$.giver");

    r.cells = {
        Val::u(id),
        Val::str(as_str(n["name"])),
        Val::str(as_str(n["summary"])),
        Val::str(as_str(n["offer_text"])),
        Val::str(as_str(n["completion_text"])),
        Val::num(as_int(n["level"])),
        Val::num(has(n, "required_level") ? as_int(n["required_level"]) : 1),
        cx.ref_cell(n, "zone", ns, file),
        Val::u(giver),
        has(n, "turn_in") ? Val::u(cx.ref(n["turn_in"], ns, file, "$.turn_in")) : Val::null(),
        has(rewards, "xp") ? Val::num(as_int(rewards["xp"])) : Val::null(),
        has(rewards, "money") ? Val::u(static_cast<std::uint64_t>(as_int(rewards["money"]))) : Val::null(),
    };

    // objectives[] -> quest_objective (oneOf by type)
    if (has(n, "objectives") && n["objectives"].IsSequence()) {
        std::uint16_t ord = 0;
        for (const auto& o : n["objectives"]) {
            const std::string type = as_str(o["type"]);
            Row& orow = new_row(cx.tbl("quest_objective"), id);
            orow.sort_key2 = std::to_string(ord);
            orow.cells = {
                Val::u(id), Val::num(ord), Val::str(type),
                has(o, "target") ? Val::u(cx.ref(o["target"], ns, file, "$.objectives[].target")) : Val::null(),
                has(o, "item") ? Val::u(cx.ref(o["item"], ns, file, "$.objectives[].item")) : Val::null(),
                has(o, "to") ? Val::u(cx.ref(o["to"], ns, file, "$.objectives[].to")) : Val::null(),
                has(o, "zone") ? Val::u(cx.ref(o["zone"], ns, file, "$.objectives[].zone")) : Val::null(),
                has(o, "poi") ? Val::str(as_str(o["poi"])) : Val::null(),
                has(o, "count") ? Val::num(as_int(o["count"])) : Val::null(),
            };
            ++ord;
        }
    }

    // prerequisites.quests[] -> quest_prereq
    if (has(n, "prerequisites") && has(n["prerequisites"], "quests") &&
        n["prerequisites"]["quests"].IsSequence()) {
        for (const auto& q : n["prerequisites"]["quests"]) {
            const std::uint32_t pq = cx.ref(q, ns, file, "$.prerequisites.quests[]");
            Row& pr = new_row(cx.tbl("quest_prereq"), id);
            pr.sort_key2 = std::to_string(pq);
            pr.cells = {Val::u(id), Val::u(pq)};
        }
    }

    // rewards.items[] (granted) + rewards.choice_items[] (choice) -> quest_reward
    auto emit_rewards = [&](const char* key, bool is_choice) {
        if (!has(rewards, key) || !rewards[key].IsSequence()) return;
        std::uint16_t ord = 0;
        for (const auto& it : rewards[key]) {
            const std::uint32_t item_id = cx.ref(it["item"], ns, file,
                                                 std::string("$.rewards.") + key + "[].item");
            Row& rr = new_row(cx.tbl("quest_reward"), id);
            rr.sort_key2 = std::string(is_choice ? "1" : "0") + "|" + std::to_string(ord);
            rr.cells = {Val::u(id), Val::boolean(is_choice), Val::num(ord), Val::u(item_id),
                        has(it, "count") ? Val::num(as_int(it["count"])) : Val::num(1)};
            ++ord;
        }
    };
    emit_rewards("items", false);
    emit_rewards("choice_items", true);
}

void emit_loot(std::uint32_t id, const YAML::Node& n, const std::string& ns,
               const std::string& file, Ctx& cx) {
    Row& r = new_row(cx.tbl("loot_table"), id);
    const IntRange money = int_range(n, "money");
    r.cells = {
        Val::u(id),
        money.present ? Val::u(static_cast<std::uint64_t>(money.min)) : Val::null(),
        money.present ? Val::u(static_cast<std::uint64_t>(money.max)) : Val::null(),
    };

    // Deterministic entry_ordinal across BOTH top-level entries[] and group
    // entries[] (loot_entry PK is (loot_table_id, entry_ordinal)); top-level
    // entries come first, then each group's entries, matching source order.
    std::uint16_t entry_ord = 0;
    auto emit_entry = [&](const YAML::Node& e, bool in_group, std::uint16_t group_ord) {
        const std::uint32_t item_id =
            has(e, "item") ? cx.ref(e["item"], ns, file, "$.entries[].item") : 0;
        const std::uint32_t nested =
            has(e, "table") ? cx.ref(e["table"], ns, file, "$.entries[].table") : 0;
        Row& er = new_row(cx.tbl("loot_entry"), id);
        er.sort_key2 = std::to_string(entry_ord);
        er.cells = {
            Val::u(id), Val::num(entry_ord),
            in_group ? Val::num(group_ord) : Val::null(),
            item_id ? Val::u(item_id) : Val::null(),
            nested ? Val::u(nested) : Val::null(),
            has(e, "chance_pct") ? Val::raw(fmt_double(as_double(e["chance_pct"]))) : Val::null(),
            has(e, "weight") ? Val::num(as_int(e["weight"])) : Val::null(),
            has(e, "quantity") && has(e["quantity"], "min") ? Val::num(as_int(e["quantity"]["min"])) : Val::null(),
            has(e, "quantity") && has(e["quantity"], "max") ? Val::num(as_int(e["quantity"]["max"])) : Val::null(),
            has(e, "quest") ? Val::u(cx.ref(e["quest"], ns, file, "$.entries[].quest")) : Val::null(),
        };
        ++entry_ord;
    };

    if (has(n, "entries") && n["entries"].IsSequence()) {
        for (const auto& e : n["entries"]) emit_entry(e, false, 0);
    }
    if (has(n, "groups") && n["groups"].IsSequence()) {
        std::uint16_t gord = 0;
        for (const auto& grp : n["groups"]) {
            Row& gr = new_row(cx.tbl("loot_group"), id);
            gr.sort_key2 = std::to_string(gord);
            gr.cells = {
                Val::u(id), Val::num(gord), Val::str(as_str(grp["name"])),
                Val::num(as_int(grp["pick"])),
                has(grp, "chance_pct") ? Val::raw(fmt_double(as_double(grp["chance_pct"]))) : Val::null(),
            };
            if (has(grp, "entries") && grp["entries"].IsSequence()) {
                for (const auto& e : grp["entries"]) emit_entry(e, true, gord);
            }
            ++gord;
        }
    }
}

void emit_vendor(std::uint32_t id, const YAML::Node& n, const std::string& ns,
                 const std::string& file, Ctx& cx) {
    Row& r = new_row(cx.tbl("vendor_inventory"), id);
    r.cells = {Val::u(id)};

    if (has(n, "items") && n["items"].IsSequence()) {
        std::uint16_t ord = 0;
        for (const auto& it : n["items"]) {
            const std::uint32_t item_id = cx.ref(it["item"], ns, file, "$.items[].item");
            const YAML::Node lim = has(it, "limited") ? it["limited"] : YAML::Node();
            Row& ir = new_row(cx.tbl("vendor_inventory_item"), id);
            ir.sort_key2 = std::to_string(ord);
            ir.cells = {
                Val::u(id), Val::num(ord), Val::u(item_id),
                has(it, "price_override") ? Val::u(static_cast<std::uint64_t>(as_int(it["price_override"]))) : Val::null(),
                has(lim, "count") ? Val::num(as_int(lim["count"])) : Val::null(),
                has(lim, "restock_minutes") ? Val::num(as_int(lim["restock_minutes"])) : Val::null(),
            };
            ++ord;
        }
    }

    if (has(n, "buys") && n["buys"].IsSequence()) {
        for (const auto& b : n["buys"]) {
            Row& br = new_row(cx.tbl("vendor_inventory_buys"), id);
            br.sort_key2 = as_str(b);
            br.cells = {Val::u(id), Val::str(as_str(b))};
        }
    }
}

void emit_zone(std::uint32_t id, const YAML::Node& n, const std::string& ns,
               const std::string& file, Ctx& cx) {
    Row& r = new_row(cx.tbl("zone"), id);
    const IntRange lvl = int_range(n, "level_range");
    const YAML::Node music = has(n, "music") ? n["music"] : YAML::Node();
    r.cells = {
        Val::u(id),
        Val::str(as_str(n["name"])),
        Val::num(lvl.min), Val::num(lvl.max),
        Val::boolean(has(n, "start_zone") ? as_bool(n["start_zone"]) : false),
        cx.ref_cell(music, "explore", ns, file),
        cx.ref_cell(music, "tension", ns, file),
        cx.ref_cell(music, "combat", ns, file),
        cx.ref_cell(n, "ambience", ns, file),
    };

    // pois[] -> area
    if (has(n, "pois") && n["pois"].IsSequence()) {
        for (const auto& p : n["pois"]) {
            const YAML::Node pos = p["position"];
            Row& ar = new_row(cx.tbl("area"), id);
            ar.sort_key2 = as_str(p["id"]);
            ar.cells = {
                Val::u(id), Val::str(as_str(p["id"])), Val::str(as_str(p["name"])),
                Val::raw(fmt_double(as_double(pos["x"]))),
                Val::raw(fmt_double(as_double(pos["y"]))),
                Val::raw(fmt_double(as_double(pos["z"]))),
                has(p, "discovery_radius_m") ? Val::raw(fmt_double(as_double(p["discovery_radius_m"]))) : Val::raw("40"),
                has(p, "discovery_xp") ? Val::num(as_int(p["discovery_xp"])) : Val::num(0),
            };
        }
    }

    // graveyards[] -> graveyard
    if (has(n, "graveyards") && n["graveyards"].IsSequence()) {
        std::uint16_t ord = 0;
        for (const auto& gy : n["graveyards"]) {
            const YAML::Node pos = gy["position"];
            Row& gr = new_row(cx.tbl("graveyard"), id);
            gr.sort_key2 = std::to_string(ord);
            gr.cells = {
                Val::u(id), Val::num(ord),
                Val::raw(fmt_double(as_double(pos["x"]))),
                Val::raw(fmt_double(as_double(pos["y"]))),
                Val::raw(fmt_double(as_double(pos["z"]))),
                has(gy, "orientation_deg") ? Val::raw(fmt_double(as_double(gy["orientation_deg"]))) : Val::raw("0"),
            };
            ++ord;
        }
    }
}

// Spawn files carry a file-level id but each spawns[] item becomes its own
// spawn_point with an IF-9 numeric id. Per SAD/DDL, "per-spawn ids are minted by
// mcc"; here the file's numeric id is the base and each spawn is base + (ord+1)
// within the file's reserved range is not modeled by idmap.lock (which only maps
// file-level ids). To keep every spawn_point.id unique + deterministic without a
// per-spawn idmap entry, we derive it from the file id and the spawn ordinal:
// spawn_point.id = file_numeric_id * 256 + ordinal is NOT safe (band overflow).
// Instead we allocate sequential ids from a caller-provided counter (see emit()).
struct SpawnCounter {
    std::uint32_t next;
};

void emit_spawn(std::uint32_t /*file_id*/, const YAML::Node& n, const std::string& ns,
                const std::string& file, Ctx& cx, SpawnCounter& counter) {
    const std::uint32_t zone_id = cx.ref(n["zone"], ns, file, "$.zone");
    if (!has(n, "spawns") || !n["spawns"].IsSequence()) return;
    for (const auto& s : n["spawns"]) {
        const std::uint32_t sp_id = counter.next++;
        const std::uint32_t npc_id = cx.ref(s["npc"], ns, file, "$.spawns[].npc");
        const YAML::Node pos = s["position"];
        const IntRange respawn = int_range(s, "respawn_seconds");
        const bool has_patrol = has(s, "patrol");
        Row& sr = new_row(cx.tbl("spawn_point"), sp_id);
        sr.cells = {
            Val::u(sp_id), Val::u(zone_id), Val::u(npc_id),
            Val::raw(fmt_double(as_double(pos["x"]))),
            Val::raw(fmt_double(as_double(pos["y"]))),
            Val::raw(fmt_double(as_double(pos["z"]))),
            has(s, "orientation_deg") ? Val::raw(fmt_double(as_double(s["orientation_deg"]))) : Val::raw("0"),
            Val::num(respawn.min), Val::num(respawn.max),
            has(s, "wander_radius_m") ? Val::raw(fmt_double(as_double(s["wander_radius_m"]))) : Val::null(),
        };

        if (has_patrol) {
            const YAML::Node patrol = s["patrol"];
            Row& pr = new_row(cx.tbl("patrol_path"), sp_id);
            pr.cells = {Val::u(sp_id),
                        Val::boolean(has(patrol, "loop") ? as_bool(patrol["loop"]) : true)};
            if (has(patrol, "waypoints") && patrol["waypoints"].IsSequence()) {
                std::uint16_t wp = 0;
                for (const auto& w : patrol["waypoints"]) {
                    const YAML::Node wpos = w["position"];
                    Row& wr = new_row(cx.tbl("patrol_waypoint"), sp_id);
                    wr.sort_key2 = std::to_string(wp);
                    wr.cells = {
                        Val::u(sp_id), Val::num(wp),
                        Val::raw(fmt_double(as_double(wpos["x"]))),
                        Val::raw(fmt_double(as_double(wpos["y"]))),
                        Val::raw(fmt_double(as_double(wpos["z"]))),
                        has(w, "wait_seconds") ? Val::raw(fmt_double(as_double(w["wait_seconds"]))) : Val::raw("0"),
                    };
                    ++wp;
                }
            }
        }
    }
}

// ---- Table registry: names + columns in DDL declaration order ---------------
// Order mirrors schema/sql/world/*.sql (10_npc .. 90_gossip) so the emitted DML
// loads in dependency order (also bracketed by FOREIGN_KEY_CHECKS=0).
std::map<std::string, Table> make_tables() {
    std::map<std::string, Table> t;
    auto add = [&](const std::string& name, std::vector<std::string> cols) {
        t[name] = Table{name, std::move(cols), {}};
    };
    add("npc_template", {"id","name","subtitle","level_min","level_max","creature_type","`rank`","faction",
        "stat_health","stat_mana","stat_armor","stat_damage_min","stat_damage_max","stat_attack_speed_ms",
        "ai_behavior","ai_aggro_radius_m","ai_leash_radius_m","ai_call_for_help_radius_m","ai_flee_at_health_pct",
        "move_walk_speed_mps","move_run_speed_mps","vendor_ref_id","loot_table_ref_id","loot_money_min",
        "loot_money_max","visual_model_id","visual_scale","visual_sound_set_id"});
    add("npc_ability", {"npc_id","ability_id","priority","cooldown_override_ms","use_at_health_below_pct"});
    add("item_template", {"id","name","flavor_text","item_class","subclass","slot","rarity","required_level",
        "item_level","is_unique","binding","stack_size","weapon_damage_min","weapon_damage_max","weapon_speed_ms",
        "weapon_school","armor","effect_on_use_id","price_sell","price_buy","visual_icon_id","visual_model_id"});
    add("item_stat", {"item_id","stat","amount"});
    add("item_effect_on_equip", {"item_id","ordinal","ability_id"});
    add("ability", {"id","name","description","school","target","range_m","cast_time_ms","cast_channel_ms",
        "cooldown_ms","triggers_gcd","resource_type","resource_amount","av_cast_anim","av_cast_vfx_id",
        "av_cast_sfx_id","av_impact_vfx_id","av_impact_sfx_id"});
    add("ability_effect", {"ability_id","ordinal","kind","amount_min","amount_max","coefficient","threat_amount",
        "duration_ms","max_stacks","periodic_kind","periodic_amount_min","periodic_amount_max","periodic_tick_ms"});
    add("ability_effect_stat_mod", {"ability_id","ordinal","stat","amount"});
    add("quest_template", {"id","name","summary","offer_text","completion_text","level","required_level",
        "zone_ref_id","giver_npc_id","turn_in_npc_id","reward_xp","reward_money"});
    add("quest_objective", {"quest_id","ordinal","type","target_npc_id","item_id","to_npc_id","zone_ref_id","poi","count"});
    add("quest_prereq", {"quest_id","prereq_quest_id"});
    add("quest_reward", {"quest_id","is_choice","ordinal","item_id","count"});
    add("loot_table", {"id","money_min","money_max"});
    add("loot_group", {"loot_table_id","ordinal","name","pick","chance_pct"});
    add("loot_entry", {"loot_table_id","entry_ordinal","group_ordinal","item_id","nested_table_id","chance_pct",
        "weight","quantity_min","quantity_max","quest_ref_id"});
    add("vendor_inventory", {"id"});
    add("vendor_inventory_item", {"vendor_id","ordinal","item_id","price_override","limited_count","limited_restock_minutes"});
    add("vendor_inventory_buys", {"vendor_id","item_class"});
    add("spawn_point", {"id","zone_ref_id","npc_id","pos_x","pos_y","pos_z","orientation_deg","respawn_min","respawn_max","wander_radius_m"});
    add("patrol_path", {"spawn_point_id","`loop`"});
    add("patrol_waypoint", {"spawn_point_id","ordinal","pos_x","pos_y","pos_z","wait_seconds"});
    add("zone", {"id","name","level_min","level_max","start_zone","music_explore_id","music_tension_id","music_combat_id","ambience_id"});
    add("area", {"zone_id","poi","name","pos_x","pos_y","pos_z","discovery_radius_m","discovery_xp"});
    add("graveyard", {"zone_id","ordinal","pos_x","pos_y","pos_z","orientation_deg"});
    add("gossip", {"npc_id","`text`"});
    return t;
}

// Emission order = DDL declaration order (10_npc .. 90_gossip). Content-table
// families in the exact order the schema loader declares them, so a plain
// `mysql < world.sql` loads without FK ordering surprises (also FK-checks off).
const std::vector<std::string> kEmitOrder = {
    "npc_template", "npc_ability",
    "item_template", "item_stat", "item_effect_on_equip",
    "ability", "ability_effect", "ability_effect_stat_mod",
    "quest_template", "quest_objective", "quest_prereq", "quest_reward",
    "loot_table", "loot_group", "loot_entry",
    "vendor_inventory", "vendor_inventory_item", "vendor_inventory_buys",
    "spawn_point", "patrol_path", "patrol_waypoint",
    "zone", "area", "graveyard",
    "gossip",
};

// Render one table's rows as a batched multi-VALUES INSERT (deterministic order).
void render_table(const Table& t, std::ostream& out) {
    if (t.rows.empty()) return;
    std::vector<const Row*> ordered;
    ordered.reserve(t.rows.size());
    for (const auto& r : t.rows) ordered.push_back(&r);
    std::stable_sort(ordered.begin(), ordered.end(), [](const Row* a, const Row* b) {
        if (a->sort_key != b->sort_key) return a->sort_key < b->sort_key;
        return a->sort_key2 < b->sort_key2;
    });

    out << "INSERT INTO " << t.name << " (";
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
        if (i) out << ", ";
        out << t.columns[i];
    }
    out << ") VALUES\n";
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        out << "  (";
        const auto& cells = ordered[i]->cells;
        for (std::size_t c = 0; c < cells.size(); ++c) {
            if (c) out << ", ";
            out << cells[c].render();
        }
        out << ")" << (i + 1 < ordered.size() ? ",\n" : ";\n");
    }
}

}  // namespace

EmitSqlResult emit_sql(const model::ContentModel& model, const LinkResult& linked,
                       const EmitSqlOptions& opts, diag::Diagnostics& diags) {
    EmitSqlResult result;
    Resolver resolver(linked.idmaps, diags);
    std::map<std::string, Table> tables = make_tables();
    Ctx cx{tables, resolver};

    // Spawn placements need per-placement numeric ids that don't collide with
    // file-level ids. They come from the top of each pack's band, growing DOWN
    // from kMaxLocalIndex so they never collide with the append-only file ids
    // growing UP from 1. (Deterministic: source order within a pack.)
    // We use the primary pack's band for all spawns (single-pack content today).
    std::uint32_t spawn_band = 0;
    for (const auto& [ns, m] : linked.idmaps) {
        if (ns == "core") { spawn_band = m.band; break; }
    }
    if (linked.idmaps.size() == 1) spawn_band = linked.idmaps.begin()->second.band;
    SpawnCounter spawn_counter{idmap::numeric_id(spawn_band, idmap::kMaxLocalIndex)};

    // ---- Walk content, dispatch by type (deterministic: sorted by string id).
    std::vector<const model::ParsedFile*> content;
    for (const auto& pf : model.files) {
        if (!pf.parsed) continue;
        if (pf.file.kind != model::FileKind::Content) continue;
        if (pf.id.empty()) continue;
        content.push_back(&pf);
    }
    std::sort(content.begin(), content.end(),
              [](const model::ParsedFile* a, const model::ParsedFile* b) {
                  return a->id < b->id;
              });

    // Resolve a file's own numeric id via its namespace's idmap.
    auto numeric_of = [&](const model::ParsedFile& pf) -> std::uint32_t {
        const std::string ns = ns_of(pf.id);
        const auto mit = linked.idmaps.find(ns);
        if (mit == linked.idmaps.end()) return 0;
        const auto iit = mit->second.map.find(pf.id);
        if (iit == mit->second.map.end()) {
            diags.error("E001", pf.file.rel_path, "$.id",
                        "emit-sql: entity '" + pf.id + "' has no IF-9 numeric id");
            return 0;
        }
        return idmap::numeric_id(mit->second.band, iit->second);
    };

    for (const model::ParsedFile* pf : content) {
        const std::uint32_t id = numeric_of(*pf);
        const std::string ns = ns_of(pf->id);
        const std::string& file = pf->file.rel_path;
        const YAML::Node& n = pf->root;
        const std::string& type = pf->file.file_type;

        if (type == "npc") emit_npc(id, n, ns, file, cx);
        else if (type == "item") emit_item(id, n, ns, file, cx);
        else if (type == "ability") emit_ability(id, n, ns, file, cx);
        else if (type == "quest") emit_quest(id, n, ns, file, cx);
        else if (type == "loot") emit_loot(id, n, ns, file, cx);
        else if (type == "vendor") emit_vendor(id, n, ns, file, cx);
        else if (type == "zone") emit_zone(id, n, ns, file, cx);
        else if (type == "spawn") emit_spawn(id, n, ns, file, cx, spawn_counter);
        // asset sidecars carry no world-DB rows (client-facing; .pck).
    }

    // ---- Compute the per-pack content hash (BLAKE3 of the canonical source
    // tree). This is the SHARED computation emit-pck also uses (content_hash.h),
    // so the IF-4 world_manifest hash and the IF-5 pack.manifest.json hash are
    // byte-identical — the three-way content-hash tie (SAD §2.6). Keeping it in
    // one place is what makes the tie structurally impossible to break.
    const std::map<std::string, std::string> pack_hash = compute_pack_hashes(model);

    // ---- Assemble the SQL script. -----------------------------------------
    std::ostringstream out;
    out << "-- world DB DML — generated by mcc emit-sql (IF-4, Tools SAD §2.6).\n"
           "-- Deterministic full dump: tables in DDL order, rows by primary key.\n"
           "-- DO NOT EDIT — rebuild with `mcc emit-sql`.\n"
           "SET NAMES utf8mb4;\n"
           "SET FOREIGN_KEY_CHECKS = 0;\n\n";

    // world_manifest: one row per pack (SAD §2.6; worldd #89 reads these seven
    // columns). Emitted first, deterministically ordered by pack_namespace.
    out << "-- world_manifest (boot handshake; worldd reads + verifies this).\n"
           "INSERT INTO world_manifest\n"
           "  (pack_namespace, pack_version, id_band, content_hash, schema_version, mcc_version, built_at)\n"
           "VALUES\n";
    // Collect pack rows (from pack manifests) sorted by namespace.
    struct PackRow {
        std::string ns, version, hash;
        std::uint32_t band = 0, schema = 1;
    };
    std::vector<PackRow> packs;
    for (const auto& pf : model.files) {
        if (!pf.parsed || pf.file.kind != model::FileKind::Pack) continue;
        PackRow pr;
        pr.ns = pf.namespace_;
        pr.version = has(pf.root, "version") ? as_str(pf.root["version"]) : "0.0.0";
        pr.schema = has(pf.root, "content_schema_version")
                        ? static_cast<std::uint32_t>(as_int(pf.root["content_schema_version"]))
                        : 1;
        const auto mit = linked.idmaps.find(pr.ns);
        pr.band = mit != linked.idmaps.end() ? mit->second.band : 0;
        const auto hit = pack_hash.find(pr.ns);
        pr.hash = hit != pack_hash.end() ? hit->second
                                         : std::string(hash::kBlake3HexLen, '0');
        packs.push_back(std::move(pr));
    }
    std::sort(packs.begin(), packs.end(),
              [](const PackRow& a, const PackRow& b) { return a.ns < b.ns; });
    if (packs.empty()) {
        diags.error("E002", "content/", "",
                    "emit-sql: no pack.yaml found — cannot emit world_manifest");
        result.ok = false;
    }
    for (std::size_t i = 0; i < packs.size(); ++i) {
        const PackRow& p = packs[i];
        out << "  (" << sql_quote(p.ns) << ", " << sql_quote(p.version) << ", " << p.band
            << ", " << sql_quote(p.hash) << ", " << p.schema << ", "
            << sql_quote(opts.mcc_version) << ", " << sql_quote(opts.built_at) << ")"
            << (i + 1 < packs.size() ? ",\n" : ";\n");
        ++result.manifest_rows;
    }
    out << "\n";

    // Content tables in DDL declaration order.
    for (const std::string& name : kEmitOrder) {
        const Table& t = tables.at(name);
        if (t.rows.empty()) continue;
        out << "-- " << t.name << " (" << t.rows.size() << " rows)\n";
        render_table(t, out);
        out << "\n";
        result.content_rows += t.rows.size();
    }

    out << "SET FOREIGN_KEY_CHECKS = 1;\n";

    result.sql = out.str();
    if (!diags.ok()) result.ok = false;
    return result;
}

}  // namespace mcc::stages
