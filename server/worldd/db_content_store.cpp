// SPDX-License-Identifier: Apache-2.0
//
// worldd — world-DB content load layer (issue #390). Implementation of the DB-backed
// content stores declared in db_content_store.h. See that header for the design and
// the trainer-abilities scope note.
//
// PARAMETERIZED SQL ONLY: every meridian::db::Connection::execute call binds values
// through prepared-statement `?` parameters (CONTRIBUTING.md backend rule). The
// SELECTs here take no user input at all (a boot-time full load), but the pattern is
// held regardless — no value is ever concatenated into a statement.
//
// Clean-room, original code (CONTRIBUTING.md).

#include "db_content_store.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

namespace meridian::worldd {
namespace {

namespace itm = meridian::items;

// --- Cell parsing helpers -----------------------------------------------------
// meridian::db returns every column as an optional<string> (Cell); numerics are
// parsed here. A NULL cell (nullopt) maps to the caller-supplied default.

std::uint64_t as_u64(const db::Cell& c, std::uint64_t dflt = 0) {
    if (!c.has_value()) return dflt;
    try {
        return static_cast<std::uint64_t>(std::stoull(*c));
    } catch (...) {
        return dflt;
    }
}

std::int64_t as_i64(const db::Cell& c, std::int64_t dflt = 0) {
    if (!c.has_value()) return dflt;
    try {
        return static_cast<std::int64_t>(std::stoll(*c));
    } catch (...) {
        return dflt;
    }
}

std::uint32_t as_u32(const db::Cell& c, std::uint32_t dflt = 0) {
    return static_cast<std::uint32_t>(as_u64(c, dflt));
}

std::uint16_t as_u16(const db::Cell& c, std::uint16_t dflt = 0) {
    return static_cast<std::uint16_t>(as_u64(c, dflt));
}

std::string as_str(const db::Cell& c) { return c.has_value() ? *c : std::string(); }

// A FLOAT/DOUBLE column, parsed from its text cell. NULL / non-numeric -> default.
// The meridian::db result layer round-trips FLOAT result columns as text since the
// #393 fix (#413), so area pos_x/y/z + discovery_radius_m read directly here.
float as_f32(const db::Cell& c, float dflt = 0.0f) {
    if (!c.has_value()) return dflt;
    try {
        return std::stof(*c);
    } catch (...) {
        return dflt;
    }
}

// An optional copper amount: NULL -> nullopt (schema "omit = unsellable / not sold").
std::optional<itm::Copper> as_opt_copper(const db::Cell& c) {
    if (!c.has_value()) return std::nullopt;
    return static_cast<itm::Copper>(as_i64(c));
}

// Clamp an already-computed basis-point value to [0, kLootRollScale] (a d10000).
// The loot queries compute the bp integer in SQL — CAST(ROUND(COALESCE(chance_pct,
// 100) * 100) AS SIGNED) — rather than read the raw chance_pct FLOAT, because the
// meridian::db result layer binds every column as a STRING and does not round-trip
// a FLOAT result value (it comes back empty); an INTEGER-typed result column does.
// A NULL chance_pct is the loot schema default of 100% (always) via the COALESCE.
std::uint32_t clamp_bp(const db::Cell& c) {
    std::int64_t bp = as_i64(c, static_cast<std::int64_t>(loot::kLootRollScale));
    if (bp < 0) bp = 0;
    if (bp > static_cast<std::int64_t>(loot::kLootRollScale)) bp = loot::kLootRollScale;
    return static_cast<std::uint32_t>(bp);
}

// --- Enum string -> C++ enum mappings (world DDL ENUM columns) ----------------

itm::ItemClass item_class_from_db(const std::string& s) {
    if (s == "weapon") return itm::ItemClass::kWeapon;
    if (s == "armor") return itm::ItemClass::kArmor;
    if (s == "consumable") return itm::ItemClass::kConsumable;
    if (s == "quest") return itm::ItemClass::kQuest;
    if (s == "trade_good") return itm::ItemClass::kTradeGood;
    if (s == "container") return itm::ItemClass::kContainer;
    return itm::ItemClass::kTradeGood;
}

itm::ItemSlot item_slot_from_db(const db::Cell& c) {
    if (!c.has_value()) return itm::ItemSlot::kNone;
    const std::string& s = *c;
    if (s == "head") return itm::ItemSlot::kHead;
    if (s == "shoulders") return itm::ItemSlot::kShoulders;
    if (s == "back") return itm::ItemSlot::kBack;
    if (s == "chest") return itm::ItemSlot::kChest;
    if (s == "wrist") return itm::ItemSlot::kWrist;
    if (s == "hands") return itm::ItemSlot::kHands;
    if (s == "waist") return itm::ItemSlot::kWaist;
    if (s == "legs") return itm::ItemSlot::kLegs;
    if (s == "feet") return itm::ItemSlot::kFeet;
    if (s == "neck") return itm::ItemSlot::kNeck;
    if (s == "finger") return itm::ItemSlot::kFinger;
    if (s == "trinket") return itm::ItemSlot::kTrinket;
    if (s == "main_hand") return itm::ItemSlot::kMainHand;
    if (s == "off_hand") return itm::ItemSlot::kOffHand;
    if (s == "two_hand") return itm::ItemSlot::kTwoHand;
    if (s == "ranged") return itm::ItemSlot::kRanged;
    if (s == "bag") return itm::ItemSlot::kBag;
    return itm::ItemSlot::kNone;
}

itm::Rarity rarity_from_db(const std::string& s) {
    if (s == "poor") return itm::Rarity::kPoor;
    if (s == "common") return itm::Rarity::kCommon;
    if (s == "uncommon") return itm::Rarity::kUncommon;
    if (s == "rare") return itm::Rarity::kRare;
    if (s == "epic") return itm::Rarity::kEpic;
    if (s == "legendary") return itm::Rarity::kLegendary;
    return itm::Rarity::kCommon;
}

itm::Binding binding_from_db(const db::Cell& c) {
    const std::string s = as_str(c);
    if (s == "on_pickup") return itm::Binding::kOnPickup;
    if (s == "on_equip") return itm::Binding::kOnEquip;
    return itm::Binding::kNone;
}

itm::StatKey stat_key_from_db(const std::string& s) {
    if (s == "strength") return itm::StatKey::kStrength;
    if (s == "agility") return itm::StatKey::kAgility;
    if (s == "stamina") return itm::StatKey::kStamina;
    if (s == "intellect") return itm::StatKey::kIntellect;
    if (s == "spirit") return itm::StatKey::kSpirit;
    return itm::StatKey::kStrength;
}

ObjectiveType objective_type_from_db(const std::string& s) {
    if (s == "kill") return ObjectiveType::kKill;
    if (s == "collect") return ObjectiveType::kCollect;
    if (s == "deliver") return ObjectiveType::kDeliver;
    if (s == "explore") return ObjectiveType::kExplore;
    return ObjectiveType::kKill;
}

// A truthy BOOLEAN/TINYINT cell ("1"/"0" from MariaDB).
bool as_bool(const db::Cell& c) { return as_i64(c) != 0; }

// npc_trainer_ability.required_class ENUM class-name -> roster Class id (roster.h:
// kVanguard=1, kRuncaller=2, kWarden=3, kMender=4). NULL/unknown => 0 (any class),
// matching npc::TrainerAbility::required_class (0 = any). Kept as plain constants
// (not a meridian::characters dependency) exactly like npc_def.h's kClassVanguard.
std::uint8_t trainer_class_from_db(const db::Cell& c) {
    if (!c.has_value()) return 0;  // NULL => any class may learn
    const std::string& s = *c;
    if (s == "vanguard") return 1;
    if (s == "runcaller") return 2;
    if (s == "warden") return 3;
    if (s == "mender") return 4;
    return 0;
}

// --- ability_* ENUM columns -> ability_store.h enums (#481) -------------------
// The world DDL `ability` / `ability_effect` / `ability_effect_stat_mod` ENUM
// string columns mapped 1:1 to the compiled model. Unknown/NULL falls back to the
// safest default (matching the DDL DEFAULTs) so a malformed pack degrades rather
// than throwing (client SAD §2.4 "never crash").

School ability_school_from_db(const std::string& s) {
    if (s == "physical") return School::kPhysical;
    if (s == "fire") return School::kFire;
    if (s == "frost") return School::kFrost;
    if (s == "nature") return School::kNature;
    if (s == "shadow") return School::kShadow;
    if (s == "holy") return School::kHoly;
    if (s == "arcane") return School::kArcane;
    return School::kPhysical;
}

TargetKind ability_target_from_db(const std::string& s) {
    if (s == "self") return TargetKind::kSelf;
    if (s == "enemy") return TargetKind::kEnemy;
    if (s == "friendly") return TargetKind::kFriendly;
    return TargetKind::kEnemy;
}

// resource_type ENUM('mana','rage','energy') NULL — NULL => kNone (a free ability).
AbilityResourceType ability_resource_from_db(const db::Cell& c) {
    if (!c.has_value()) return AbilityResourceType::kNone;
    const std::string& s = *c;
    if (s == "mana") return AbilityResourceType::kMana;
    if (s == "rage") return AbilityResourceType::kRage;
    if (s == "energy") return AbilityResourceType::kEnergy;
    return AbilityResourceType::kNone;
}

EffectKind ability_effect_kind_from_db(const std::string& s) {
    if (s == "damage") return EffectKind::kDamage;
    if (s == "heal") return EffectKind::kHeal;
    if (s == "aura") return EffectKind::kAura;
    if (s == "threat") return EffectKind::kThreat;
    return EffectKind::kDamage;
}

// periodic_kind ENUM('damage','heal') NULL — NULL => kNone (aura has no tick).
PeriodicKind ability_periodic_from_db(const db::Cell& c) {
    if (!c.has_value()) return PeriodicKind::kNone;
    const std::string& s = *c;
    if (s == "damage") return PeriodicKind::kDamage;
    if (s == "heal") return PeriodicKind::kHeal;
    return PeriodicKind::kNone;
}

// ability_effect_stat_mod.stat ENUM -> the aura StatKey (worldd::StatKey; distinct
// from items::StatKey used by item_stat above, though the value names coincide).
StatKey ability_stat_from_db(const std::string& s) {
    if (s == "strength") return StatKey::kStrength;
    if (s == "agility") return StatKey::kAgility;
    if (s == "stamina") return StatKey::kStamina;
    if (s == "intellect") return StatKey::kIntellect;
    if (s == "spirit") return StatKey::kSpirit;
    return StatKey::kStrength;
}

// A composite (ability_id, ordinal) key for locating an already-built effect when
// its stat-mod children stream in (ordinal is SMALLINT — max 4 per ability).
std::uint64_t effect_key(AbilityId ability_id, std::uint32_t ordinal) {
    return (static_cast<std::uint64_t>(ability_id) << 20) | ordinal;
}

}  // namespace

// =============================================================================
// DbTemplateStore
// =============================================================================
DbTemplateStore::DbTemplateStore(db::Connection& world_db) {
    // item_template scalars (+ weapon block + prices). One row per template.
    db::Result items = world_db.execute(
        "SELECT id, name, item_class, slot, rarity, required_level, item_level, "
        "       is_unique, binding, stack_size, "
        "       weapon_damage_min, weapon_damage_max, weapon_speed_ms, armor, "
        "       price_sell, price_buy "
        "FROM item_template ORDER BY id");
    for (const db::Row& r : items.rows) {
        itm::ItemTemplate t;
        t.id = as_u32(r[0]);
        t.name = as_str(r[1]);
        t.item_class = item_class_from_db(as_str(r[2]));
        t.slot = item_slot_from_db(r[3]);
        t.rarity = rarity_from_db(as_str(r[4]));
        t.required_level = as_u16(r[5], 1);
        t.item_level = as_u16(r[6], 1);
        t.unique = as_bool(r[7]);
        t.binding = binding_from_db(r[8]);
        t.max_stack = as_u16(r[9], 1);
        if (t.item_class == itm::ItemClass::kWeapon && r[10].has_value()) {
            itm::WeaponData w;
            w.damage_min = as_u32(r[10]);
            w.damage_max = as_u32(r[11]);
            w.speed_ms = as_u32(r[12]);
            t.weapon = w;
        }
        t.armor = as_u32(r[13]);
        t.sell_price = as_opt_copper(r[14]);
        t.buy_price = as_opt_copper(r[15]);
        by_id_.emplace(t.id, std::move(t));
    }

    // item_stat children (statKey + signed amount). Appended to their template.
    db::Result stats =
        world_db.execute("SELECT item_id, stat, amount FROM item_stat ORDER BY item_id, stat");
    for (const db::Row& r : stats.rows) {
        auto it = by_id_.find(as_u32(r[0]));
        if (it == by_id_.end()) continue;  // orphan stat (FK guarantees none) — skip
        itm::StatMod sm;
        sm.stat = stat_key_from_db(as_str(r[1]));
        sm.amount = static_cast<std::int32_t>(as_i64(r[2]));
        it->second.stats.push_back(sm);
    }
}

const itm::ItemTemplate* DbTemplateStore::find(std::uint32_t template_id) const {
    auto it = by_id_.find(template_id);
    return it == by_id_.end() ? nullptr : &it->second;
}

std::vector<std::uint32_t> DbTemplateStore::ids() const {
    std::vector<std::uint32_t> out;
    out.reserve(by_id_.size());
    for (const auto& [id, _] : by_id_) out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

// =============================================================================
// DbQuestStore
// =============================================================================
DbQuestStore::DbQuestStore(db::Connection& world_db) {
    // Quest scalar fields.
    db::Result quests = world_db.execute(
        "SELECT id, name, level, required_level, giver_npc_id, turn_in_npc_id, "
        "       reward_xp, reward_money "
        "FROM quest_template ORDER BY id");
    for (const db::Row& r : quests.rows) {
        QuestDef q;
        q.id = as_u32(r[0]);
        q.name = as_str(r[1]);
        q.level = as_u16(r[2], 1);
        q.required_level = as_u16(r[3], 1);
        q.giver_npc_id = as_u32(r[4]);
        q.turn_in_npc_id = as_u32(r[5]);  // 0 (NULL) => turn_in defaults to giver
        q.reward_xp = as_u32(r[6]);
        q.reward_money = static_cast<items::Copper>(as_i64(r[7]));
        by_id_.emplace(q.id, std::move(q));
    }

    // Objectives (ordered by ordinal). oneOf discriminated by `type`.
    db::Result objs = world_db.execute(
        "SELECT quest_id, ordinal, type, target_npc_id, item_id, to_npc_id, "
        "       zone_ref_id, poi, count "
        "FROM quest_objective ORDER BY quest_id, ordinal");
    for (const db::Row& r : objs.rows) {
        auto it = by_id_.find(as_u32(r[0]));
        if (it == by_id_.end()) continue;
        QuestObjective o;
        o.type = objective_type_from_db(as_str(r[2]));
        o.target_npc_id = as_u32(r[3]);
        o.item_id = as_u32(r[4]);
        o.to_npc_id = as_u32(r[5]);
        o.zone_id = as_u32(r[6]);
        o.poi = as_str(r[7]);
        o.count = as_u16(r[8], 1);
        it->second.objectives.push_back(std::move(o));
    }

    // Prerequisites (ALL required before accept).
    db::Result prereqs = world_db.execute(
        "SELECT quest_id, prereq_quest_id FROM quest_prereq ORDER BY quest_id, prereq_quest_id");
    for (const db::Row& r : prereqs.rows) {
        auto it = by_id_.find(as_u32(r[0]));
        if (it == by_id_.end()) continue;
        it->second.prerequisites.push_back(as_u32(r[1]));
    }

    // Rewards: is_choice=FALSE -> items[] (always granted); TRUE -> choice_items[].
    db::Result rewards = world_db.execute(
        "SELECT quest_id, is_choice, ordinal, item_id, count "
        "FROM quest_reward ORDER BY quest_id, is_choice, ordinal");
    for (const db::Row& r : rewards.rows) {
        auto it = by_id_.find(as_u32(r[0]));
        if (it == by_id_.end()) continue;
        QuestRewardItem ri;
        ri.item_id = as_u32(r[3]);
        ri.count = as_u16(r[4], 1);
        if (as_bool(r[1])) {
            it->second.choice_items.push_back(ri);
        } else {
            it->second.reward_items.push_back(ri);
        }
    }
}

const QuestDef* DbQuestStore::find(QuestId quest_id) const {
    auto it = by_id_.find(quest_id);
    return it == by_id_.end() ? nullptr : &it->second;
}

std::vector<QuestId> DbQuestStore::ids() const {
    std::vector<QuestId> out;
    out.reserve(by_id_.size());
    for (const auto& [id, _] : by_id_) out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

// =============================================================================
// DbNpcStore
// =============================================================================
DbNpcStore::DbNpcStore(db::Connection& world_db) {
    // npc_template: id, name, and the vendor role FLAG (vendor_ref_id present). The
    // trainer role (is_trainer + trainer_abilities) is filled from npc_trainer_ability
    // in a later pass below (#392); is_trainer starts false and flips true iff a
    // taught-ability row names this NPC.
    db::Result npcs = world_db.execute(
        "SELECT id, name, vendor_ref_id FROM npc_template ORDER BY id");
    for (const db::Row& r : npcs.rows) {
        npc::NpcDef d;
        d.id = as_u32(r[0]);
        d.name = as_str(r[1]);
        d.is_vendor = r[2].has_value();  // vendor_ref_id set => the vendor gossip flag
        d.is_trainer = false;
        by_id_.emplace(d.id, std::move(d));
    }

    // Quest giver / turn-in participation lives on the quest side (quest_template's
    // giver_npc_id / turn_in_npc_id). Cross-reference it onto each NPC so the gossip
    // planner sees the quest options. A single pass over quest_template, merging the
    // gives/turn_in flags per (npc, quest).
    db::Result links = world_db.execute(
        "SELECT id, giver_npc_id, turn_in_npc_id FROM quest_template ORDER BY id");
    for (const db::Row& r : links.rows) {
        const std::uint32_t quest_id = as_u32(r[0]);
        const std::uint32_t giver = as_u32(r[1]);
        // turn_in_npc_id NULL => defaults to the giver (quest.schema.yaml).
        const std::uint32_t turn_in = r[2].has_value() ? as_u32(r[2]) : giver;

        auto add_ref = [&](std::uint32_t npc_id, bool gives, bool turns_in) {
            if (npc_id == 0) return;
            auto it = by_id_.find(npc_id);
            if (it == by_id_.end()) return;  // an NPC not in npc_template — skip
            auto& refs = it->second.quests;
            auto existing = std::find_if(refs.begin(), refs.end(),
                                         [&](const npc::NpcQuestRef& q) { return q.quest_id == quest_id; });
            if (existing == refs.end()) {
                npc::NpcQuestRef ref;
                ref.quest_id = quest_id;
                ref.gives = gives;
                ref.turn_in = turns_in;
                refs.push_back(ref);
            } else {
                existing->gives = existing->gives || gives;
                existing->turn_in = existing->turn_in || turns_in;
            }
        };
        add_ref(giver, /*gives=*/true, /*turns_in=*/giver == turn_in);
        if (turn_in != giver) add_ref(turn_in, /*gives=*/false, /*turns_in=*/true);
    }

    // Trainer role (npc_trainer_ability, #392): each row is one ability this NPC
    // teaches, with its copper cost + class/level gate (the taught set the live #388
    // TRAINER_LIST/TRAINER_LEARN path reads). Any row naming an NPC makes that NPC a
    // trainer (is_trainer=true). cost_copper + required_level are INTEGER columns (read
    // directly, no FLOAT round-trip); required_class is the ENUM class-name column
    // mapped to the roster Class id (NULL => any class). The npc_trainer parent table
    // is the FK anchor + the "is a trainer with no abilities yet" marker; the ability
    // rows are what populate the def, so the query is over npc_trainer_ability.
    db::Result trainer = world_db.execute(
        "SELECT npc_id, ability_id, cost_copper, required_class, required_level "
        "FROM npc_trainer_ability ORDER BY npc_id, ability_id");
    for (const db::Row& r : trainer.rows) {
        auto it = by_id_.find(as_u32(r[0]));
        if (it == by_id_.end()) continue;  // taught ability for an unknown npc — skip
        npc::TrainerAbility ta;
        ta.ability_id = as_u32(r[1]);
        ta.cost = static_cast<items::Copper>(as_i64(r[2]));
        ta.required_class = trainer_class_from_db(r[3]);
        ta.required_level = as_u16(r[4], 1);
        it->second.is_trainer = true;
        it->second.trainer_abilities.push_back(ta);
    }
}

const npc::NpcDef* DbNpcStore::find(npc::NpcId npc_id) const {
    auto it = by_id_.find(npc_id);
    return it == by_id_.end() ? nullptr : &it->second;
}

std::vector<npc::NpcId> DbNpcStore::ids() const {
    std::vector<npc::NpcId> out;
    out.reserve(by_id_.size());
    for (const auto& [id, _] : by_id_) out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

// =============================================================================
// DbLootTableStore  (keyed by creature template id)
// =============================================================================
DbLootTableStore::DbLootTableStore(db::Connection& world_db) {
    // Assemble each loot_table (money + groups + entries) by loot_table.id first,
    // then map creature template ids to their table via npc_template.loot_table_ref_id.

    // 1. Table money ranges.
    struct TableBuild {
        loot::LootTable table;
        // group ordinal -> index into table.groups (for top-level & group entries).
        std::unordered_map<std::uint32_t, std::size_t> group_index;
    };
    std::unordered_map<std::uint32_t, TableBuild> tables;  // loot_table.id -> build

    db::Result trows =
        world_db.execute("SELECT id, money_min, money_max FROM loot_table ORDER BY id");
    for (const db::Row& r : trows.rows) {
        TableBuild tb;
        tb.table.money_min = static_cast<items::Copper>(as_i64(r[1]));
        tb.table.money_max = static_cast<items::Copper>(as_i64(r[2]));
        tables.emplace(as_u32(r[0]), std::move(tb));
    }

    // 2. Groups (mutually-exclusive weighted picks). Each becomes a LootGroup with
    //    chance_bp from chance_pct.
    db::Result grows = world_db.execute(
        "SELECT loot_table_id, ordinal, "
        "       CAST(ROUND(COALESCE(chance_pct, 100) * 100) AS SIGNED) AS chance_bp "
        "FROM loot_group ORDER BY loot_table_id, ordinal");
    for (const db::Row& r : grows.rows) {
        auto it = tables.find(as_u32(r[0]));
        if (it == tables.end()) continue;
        loot::LootGroup g;
        g.chance_bp = clamp_bp(r[2]);
        it->second.table.groups.push_back(std::move(g));
        it->second.group_index[as_u32(r[1])] = it->second.table.groups.size() - 1;
    }

    // 3. Entries. group_ordinal NULL => a top-level independent roll, modelled as its
    //    OWN single-entry group (chance_bp = the entry's chance_pct); group_ordinal set
    //    => a member of that group (uses weight). Only ITEM entries are loaded — nested
    //    table refs (nested_table_id) are an M2 concern the seam does not model, skipped.
    db::Result erows = world_db.execute(
        "SELECT loot_table_id, entry_ordinal, group_ordinal, item_id, nested_table_id, "
        "       CAST(ROUND(COALESCE(chance_pct, 100) * 100) AS SIGNED) AS chance_bp, "
        "       weight, quantity_min, quantity_max, quest_ref_id "
        "FROM loot_entry ORDER BY loot_table_id, entry_ordinal");
    for (const db::Row& r : erows.rows) {
        auto it = tables.find(as_u32(r[0]));
        if (it == tables.end()) continue;
        if (!r[3].has_value()) continue;  // nested-table entry (no item) — skip at M1

        loot::LootEntry e;
        e.item_template_id = as_u32(r[3]);
        e.min_qty = as_u32(r[7], 1);
        e.max_qty = std::max(e.min_qty, as_u32(r[8], e.min_qty ? e.min_qty : 1));
        e.weight = as_u32(r[6], 1);
        e.required_quest_id = as_u32(r[9]);

        if (r[2].has_value()) {
            // Member of an existing group.
            auto gi = it->second.group_index.find(as_u32(r[2]));
            if (gi == it->second.group_index.end()) continue;
            it->second.table.groups[gi->second].entries.push_back(std::move(e));
        } else {
            // Top-level independent roll -> its own single-entry group.
            loot::LootGroup g;
            g.chance_bp = clamp_bp(r[5]);  // r[5] is the SQL-computed chance_bp integer
            e.weight = 1;  // sole member of its group
            g.entries.push_back(std::move(e));
            it->second.table.groups.push_back(std::move(g));
        }
    }

    // 4. Map creatures -> their loot table, adding the creature's own loot_money range
    //    (D-25: npc loot.money is ADDITIVE with the referenced table's money).
    db::Result crows = world_db.execute(
        "SELECT id, loot_table_ref_id, loot_money_min, loot_money_max "
        "FROM npc_template WHERE loot_table_ref_id IS NOT NULL "
        "   OR loot_money_min IS NOT NULL ORDER BY id");
    for (const db::Row& r : crows.rows) {
        const std::uint32_t creature_id = as_u32(r[0]);
        loot::LootTable table;
        if (r[1].has_value()) {
            auto it = tables.find(as_u32(r[1]));
            if (it != tables.end()) table = it->second.table;
        }
        // Additive creature money (D-25).
        table.money_min += static_cast<items::Copper>(as_i64(r[2]));
        table.money_max += static_cast<items::Copper>(as_i64(r[3]));
        if (table.money_max < table.money_min) table.money_max = table.money_min;
        by_creature_.emplace(creature_id, std::move(table));
    }
}

const loot::LootTable* DbLootTableStore::find(std::uint32_t creature_template_id) const {
    auto it = by_creature_.find(creature_template_id);
    return it == by_creature_.end() ? nullptr : &it->second;
}

std::vector<std::uint32_t> DbLootTableStore::ids() const {
    std::vector<std::uint32_t> out;
    out.reserve(by_creature_.size());
    for (const auto& [id, _] : by_creature_) out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

// =============================================================================
// DbVendorCatalog
// =============================================================================
DbVendorCatalog::DbVendorCatalog(db::Connection& world_db) {
    // Register every vendor (so a vendor with an empty inventory still resolves to a
    // (possibly empty) listing vector, not nullptr).
    db::Result vends = world_db.execute("SELECT id FROM vendor_inventory ORDER BY id");
    for (const db::Row& r : vends.rows) {
        by_vendor_.emplace(as_u32(r[0]), std::vector<vendor::VendorListing>{});
    }

    // Listings in catalog (ordinal) order.
    db::Result items = world_db.execute(
        "SELECT vendor_id, item_id, price_override, limited_count, limited_restock_minutes "
        "FROM vendor_inventory_item ORDER BY vendor_id, ordinal");
    for (const db::Row& r : items.rows) {
        auto it = by_vendor_.find(as_u32(r[0]));
        if (it == by_vendor_.end()) continue;
        vendor::VendorListing l;
        l.item_template_id = as_u32(r[1]);
        if (r[2].has_value()) l.price_override = static_cast<items::Copper>(as_i64(r[2]));
        if (r[3].has_value()) {
            vendor::LimitedStock stock;
            stock.count = as_u32(r[3]);
            stock.restock_minutes = as_u32(r[4]);
            l.limited = stock;
        }
        it->second.push_back(std::move(l));
    }
}

const std::vector<vendor::VendorListing>* DbVendorCatalog::listings(
    std::uint32_t vendor_id) const {
    auto it = by_vendor_.find(vendor_id);
    return it == by_vendor_.end() ? nullptr : &it->second;
}

std::vector<std::uint32_t> DbVendorCatalog::ids() const {
    std::vector<std::uint32_t> out;
    out.reserve(by_vendor_.size());
    for (const auto& [id, _] : by_vendor_) out.push_back(id);
    std::sort(out.begin(), out.end());
    return out;
}

// =============================================================================
// load_area_trigger_volumes  (area / POI rows -> discovery TriggerVolumes)
// =============================================================================
std::vector<TriggerVolume> load_area_trigger_volumes(db::Connection& world_db) {
    // One discovery volume per authored POI. ORDER BY (zone_id, poi) gives a
    // deterministic id assignment + event order (the `area` PK has no numeric id, so
    // ids are synthesized 1..N in this order). pos_* + discovery_radius_m are FLOAT
    // (readable directly since #413) — the box is the POI centre inflated by the
    // discovery radius on (x, y); z spans full range (the M1 flat map is a single
    // plane, matching placeholder_area_triggers()).
    db::Result areas = world_db.execute(
        "SELECT zone_id, poi, pos_x, pos_y, pos_z, discovery_radius_m "
        "FROM area ORDER BY zone_id, poi");

    std::vector<TriggerVolume> volumes;
    volumes.reserve(areas.rows.size());
    TriggerId next_id = 1;
    for (const db::Row& r : areas.rows) {
        TriggerVolume v;
        v.id = next_id++;
        v.kind = TriggerKind::kDiscovery;   // POI discovery (POI_DISCOVERED + explore credit)
        v.area_id = as_u32(r[0]);           // zone_id — the explore-objective join key
        v.name_id = 0;                      // `area.name` is text; no idmap numeric id here
        v.poi = as_str(r[1]);               // authored zone-local POI id (the join key)
        const float cx = as_f32(r[2]);
        const float cy = as_f32(r[3]);
        (void)as_f32(r[4]);                 // pos_z: ignored — z spans full range (flat map)
        const float radius = as_f32(r[5], 40.0f);  // schema default 40 m
        v.min_x = cx - radius;
        v.max_x = cx + radius;
        v.min_y = cy - radius;
        v.max_y = cy + radius;
        // min_z / max_z keep their full-range defaults (TriggerVolume) — the flat map
        // ignores z, so a POI's pos_z does not gate membership at M1.
        volumes.push_back(std::move(v));
    }
    return volumes;
}

// =============================================================================
// load_db_ability_store  (ability / ability_effect / ability_effect_stat_mod)
// =============================================================================
// Reads the authored ability catalog from the world DB into an AbilityStore keyed
// by the IF-9 numeric id (schema/sql/world/30_ability.sql). This is the read path
// #390 left open (it covered npc/quest/loot/vendor but NOT abilities), so the live
// cast path (world_dispatch.cpp CAST_REQUEST handler) resolved authored ids like
// minor_healing=1 / pickaxe_slam=2 against the placeholder store's synthetic
// 0xF000_0000 band and answered UNKNOWN_ABILITY (#481). Loading the authored ids
// here makes ctx.abilities->find(1) resolve, so the cast starts.
//
// Three passes mirror the DbLootTableStore assembly: ability scalars, then the
// effects[] children (ordered by ordinal), then the aura stat_mods[] grandchildren
// located by (ability_id, ordinal). FLOAT columns (range_m, coefficient) read
// directly via as_f32 (the #413 FLOAT-as-text round-trip fix is merged).
AbilityStore load_db_ability_store(db::Connection& world_db) {
    std::vector<Ability> abilities;
    std::unordered_map<AbilityId, std::size_t> index;  // ability id -> abilities[] idx

    // 1. ability scalars. One row per ability.
    db::Result arows = world_db.execute(
        "SELECT id, name, school, target, range_m, cast_time_ms, cast_channel_ms, "
        "       cooldown_ms, triggers_gcd, resource_type, resource_amount "
        "FROM ability ORDER BY id");
    abilities.reserve(arows.rows.size());
    for (const db::Row& r : arows.rows) {
        Ability a;
        a.id = as_u32(r[0]);
        a.name = as_str(r[1]);
        a.school = ability_school_from_db(as_str(r[2]));
        a.target = ability_target_from_db(as_str(r[3]));
        a.range_m = as_f32(r[4], 5.0f);  // DDL DEFAULT 5
        a.cast_time_ms = as_u32(r[5]);   // NULL/0 => instant
        a.cast_channel_ms = as_u32(r[6]);
        a.cooldown_ms = as_u32(r[7]);
        a.triggers_gcd = r[8].has_value() ? as_bool(r[8]) : true;  // DDL DEFAULT TRUE
        a.resource_type = ability_resource_from_db(r[9]);
        a.resource_amount = as_u32(r[10]);
        index.emplace(a.id, abilities.size());
        abilities.push_back(std::move(a));
    }

    // 2. effects[] (1..4, in ordinal order). Record each effect's slot so its aura
    //    stat_mods can find it in pass 3.
    struct EffectSlot { std::size_t ability_idx; std::size_t effect_idx; };
    std::unordered_map<std::uint64_t, EffectSlot> effect_slots;
    db::Result erows = world_db.execute(
        "SELECT ability_id, ordinal, kind, amount_min, amount_max, coefficient, "
        "       threat_amount, duration_ms, max_stacks, periodic_kind, "
        "       periodic_amount_min, periodic_amount_max, periodic_tick_ms "
        "FROM ability_effect ORDER BY ability_id, ordinal");
    for (const db::Row& r : erows.rows) {
        auto it = index.find(as_u32(r[0]));
        if (it == index.end()) continue;  // effect for an unknown ability — skip
        Ability& a = abilities[it->second];
        AbilityEffect e;
        e.kind = ability_effect_kind_from_db(as_str(r[2]));
        e.amount_min = as_u32(r[3]);
        e.amount_max = as_u32(r[4]);
        e.coefficient = as_f32(r[5]);
        e.threat_amount = static_cast<std::int32_t>(as_i64(r[6]));
        e.duration_ms = as_u32(r[7]);
        e.max_stacks = as_u16(r[8], 1);  // DDL DEFAULT 1
        e.periodic_kind = ability_periodic_from_db(r[9]);
        e.periodic_amount_min = as_u32(r[10]);
        e.periodic_amount_max = as_u32(r[11]);
        e.periodic_tick_ms = as_u32(r[12]);
        a.effects.push_back(std::move(e));
        effect_slots.emplace(effect_key(as_u32(r[0]), as_u32(r[1])),
                             EffectSlot{it->second, a.effects.size() - 1});
    }

    // 3. aura stat_mods[] — attach each to its parent effect by (ability_id, ordinal).
    db::Result srows = world_db.execute(
        "SELECT ability_id, ordinal, stat, amount "
        "FROM ability_effect_stat_mod ORDER BY ability_id, ordinal, stat");
    for (const db::Row& r : srows.rows) {
        auto it = effect_slots.find(effect_key(as_u32(r[0]), as_u32(r[1])));
        if (it == effect_slots.end()) continue;  // stat mod for an unknown effect — skip
        StatMod sm;
        sm.stat = ability_stat_from_db(as_str(r[2]));
        sm.amount = static_cast<std::int32_t>(as_i64(r[3]));
        abilities[it->second.ability_idx]
            .effects[it->second.effect_idx]
            .stat_mods.push_back(sm);
    }

    // A duplicate id is a content fault (mcc/IF-9 assign unique ids) — from_abilities
    // drops the later row first-wins rather than throwing, so worldd still boots.
    return AbilityStore::from_abilities(abilities);
}

// =============================================================================
// load_spawn_points  (spawn_point rows resolved against npc_template)
// =============================================================================
namespace {

// npc_template.faction ENUM('friendly','neutral','hostile') -> combat Faction. An
// unknown / NULL value falls back to neutral (attackable by no one — a safe default
// for a placement whose faction is missing). 'friendly' is the quest-giver/gossip
// faction; 'hostile' is a mob (a kill objective).
Faction npc_faction_from_db(const db::Cell& c) {
    if (!c.has_value()) return Faction::kNeutral;
    const std::string& s = *c;
    if (s == "friendly") return Faction::kFriendly;
    if (s == "hostile") return Faction::kHostile;
    return Faction::kNeutral;
}

}  // namespace

std::vector<SpawnPlacement> load_spawn_points(db::Connection& world_db) {
    // PASS 1 — the placements themselves (spawn_point only). Read first + alone so an
    // EMPTY spawn_point table (the DB-content unit tests, which seed no spawns) never
    // forces the extended npc_template columns pass 2 selects to exist. ORDER BY id
    // keeps the spawn order deterministic. FLOAT pos/orientation read directly (#413).
    db::Result rows = world_db.execute(
        "SELECT id, npc_id, pos_x, pos_y, pos_z, orientation_deg, "
        "       respawn_min, respawn_max, wander_radius_m "
        "FROM spawn_point ORDER BY id");
    if (rows.rows.empty()) return {};

    std::vector<SpawnPlacement> spawns;
    spawns.reserve(rows.rows.size());
    for (const db::Row& r : rows.rows) {
        SpawnPlacement sp;
        sp.npc_id = as_u32(r[1]);
        sp.pos.x = as_f32(r[2]);
        sp.pos.y = as_f32(r[3]);
        sp.pos.z = as_f32(r[4]);
        sp.orientation_deg = as_f32(r[5]);
        sp.respawn_min = as_u32(r[6]);
        sp.respawn_max = as_u32(r[7]);
        if (r[8].has_value()) sp.wander_radius_m = as_f32(r[8]);
        spawns.push_back(std::move(sp));
    }

    // PASS 2 — resolve each placement's npc_template combat identity (name, level,
    // faction, health, mana). Only reached when at least one placement exists (so the
    // extended npc_template columns are never referenced on an empty spawn set). One
    // query over the whole template table -> a lookup map, so N spawns of one template
    // read the row once. level uses level_min (the low end of the intRange band —
    // level_max scaling is a later AI/content concern); max_health = stat_health;
    // resource is Mana when stat_mana is present (a caster NPC), else none.
    db::Result tpl = world_db.execute(
        "SELECT id, name, level_min, faction, stat_health, stat_mana FROM npc_template");
    struct TemplateCombat {
        std::string name;
        std::uint16_t level = 1;
        Faction faction = Faction::kNeutral;
        std::uint32_t health = 1;
        std::optional<std::uint32_t> mana;
    };
    std::unordered_map<std::uint32_t, TemplateCombat> by_id;
    by_id.reserve(tpl.rows.size());
    for (const db::Row& r : tpl.rows) {
        TemplateCombat tc;
        tc.name = as_str(r[1]);
        tc.level = as_u16(r[2], 1);
        tc.faction = npc_faction_from_db(r[3]);
        tc.health = as_u32(r[4], 1);
        if (r[5].has_value()) tc.mana = as_u32(r[5]);
        by_id.emplace(as_u32(r[0]), std::move(tc));
    }

    for (SpawnPlacement& sp : spawns) {
        auto it = by_id.find(sp.npc_id);
        if (it == by_id.end()) continue;  // a spawn for an unknown template — leave defaults
        const TemplateCombat& tc = it->second;
        sp.name = tc.name;
        sp.stats.level = tc.level;
        sp.stats.max_health = tc.health == 0 ? 1 : tc.health;  // a live Unit has >= 1 HP
        sp.stats.faction = tc.faction;
        if (tc.mana) {
            sp.stats.resource_type = ResourceType::kMana;
            sp.stats.max_resource = *tc.mana;
        }
    }
    return spawns;
}

// =============================================================================
// load_world_content
// =============================================================================
WorldContent load_world_content(db::Connection& world_db) {
    WorldContent content;
    content.items = std::make_unique<DbTemplateStore>(world_db);
    content.vendor = std::make_unique<DbVendorCatalog>(world_db);
    content.quests = std::make_unique<DbQuestStore>(world_db);
    content.npcs = std::make_unique<DbNpcStore>(world_db);
    content.loot = std::make_unique<DbLootTableStore>(world_db);
    content.area_triggers = load_area_trigger_volumes(world_db);
    content.abilities = std::make_unique<AbilityStore>(load_db_ability_store(world_db));
    content.spawns = load_spawn_points(world_db);
    return content;
}

}  // namespace meridian::worldd
