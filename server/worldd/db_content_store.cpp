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
    // npc_template: id, name, and the vendor role FLAG (vendor_ref_id present).
    // Trainer abilities have no world-DDL home yet (deferred mcc #28); is_trainer
    // stays false and trainer_abilities empty for every DB-loaded NPC.
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
// load_world_content
// =============================================================================
WorldContent load_world_content(db::Connection& world_db) {
    WorldContent content;
    content.items = std::make_unique<DbTemplateStore>(world_db);
    content.vendor = std::make_unique<DbVendorCatalog>(world_db);
    content.quests = std::make_unique<DbQuestStore>(world_db);
    content.npcs = std::make_unique<DbNpcStore>(world_db);
    content.loot = std::make_unique<DbLootTableStore>(world_db);
    return content;
}

}  // namespace meridian::worldd
