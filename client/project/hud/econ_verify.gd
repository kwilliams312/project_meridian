# SPDX-License-Identifier: Apache-2.0
#
# Project Meridian — headless runtime verification for the LOOT + VENDOR + TRAINER + BAGS
# UI (ITM-01/02, ECO-01, NPC-02; #441): the event-bus loot/vendor/trainer/currency
# registry, the four windows, and the MeridianNetThread econ frame builders + decode
# safety. NOT a shipped scene: run it as
#   godot --headless --script res://hud/econ_verify.gd
# so CI / a dev box proves — with NO display and NO server — that:
#   * the bus stores + re-emits the server's inventory/loot/vendor/trainer state
#     (INVENTORY_SNAPSHOT → real item list + money known from spawn; VENDOR_LIST → the buy
#     catalog; LOOT_RESPONSE → take removes a slot; a money take clears the pile; vendor
#     buy/sell/buyback apply the server balance + drive the buyback queue via the ECHOED
#     buyback_slot; TRAINER_LIST + learn mark abilities known; currency is server-absolute);
#   * the four windows render that state (bags item grid + money, real vendor catalog, loot
#     rows, trainer rows, buyback tab) and issue the right INTENTS back through the bus
#     (take / release / buy from a catalog row / sell / buyback / learn);
#   * the money formatter splits copper into g/s/c correctly;
#   * MeridianNetThread builds the C→S econ frames non-empty and its decode_econ_frame
#     rejects a garbage body safely (kind "") — the full wire round-trip is proven by the
#     C++ ctest (client/net/test — loot/vendor/trainer codec cases).
# Exits 0 on success, 1 on any failed assertion — same shape as quest_verify.gd.
extends SceneTree

const EventBus := preload("res://hud/event_bus.gd")
const LootWindow := preload("res://hud/loot_window.gd")
const VendorWindow := preload("res://hud/vendor_window.gd")
const TrainerWindow := preload("res://hud/trainer_window.gd")
const BagsWindow := preload("res://hud/bags_window.gd")
const Money := preload("res://hud/money.gd")

var _fails := 0


func _check(name: String, ok: bool) -> void:
	print("  [%s] %s" % ["PASS" if ok else "FAIL", name])
	if not ok:
		_fails += 1


func _wait(frames: int) -> void:
	for _i in range(frames):
		await process_frame


func _initialize() -> void:
	print("meridian INVENTORY + LOOT + VENDOR + TRAINER + BAGS UI RUNTIME verify (#441/#471)")

	_verify_money_format()
	_verify_event_bus_loot()
	_verify_event_bus_inventory()
	_verify_event_bus_vendor()
	_verify_event_bus_vendor_list()
	_verify_event_bus_trainer()
	await _verify_loot_window()
	await _verify_trainer_window()
	await _verify_vendor_window()
	await _verify_bags_window()
	_verify_net_bridge()

	print("\n%d failure(s)" % _fails)
	quit(1 if _fails > 0 else 0)


# --- Money formatting --------------------------------------------------------

func _verify_money_format() -> void:
	print("[money]")
	_check("0 copper → '0c'", Money.format_copper(0) == "0c")
	_check("5 copper → '5c'", Money.format_copper(5) == "5c")
	_check("137 copper → '1s 37c'", Money.format_copper(137) == "1s 37c")
	_check("10000 copper → '1g'", Money.format_copper(10000) == "1g")
	_check("12345 copper → '1g 23s 45c'", Money.format_copper(12345) == "1g 23s 45c")
	_check("20000 copper → '2g'", Money.format_copper(20000) == "2g")


# --- Event bus: loot ---------------------------------------------------------

func _verify_event_bus_loot() -> void:
	print("[event_bus/loot]")
	var bus = EventBus.new()

	var win_events: Array = []
	bus.loot_window_changed.connect(func(c, s, cop, it): win_events.append([c, s, cop, it]))
	var items := [
		{"slot": 0, "item_template_id": 1201, "count": 2, "quality": 1, "quest_item": false},
		{"slot": 1, "item_template_id": 9002, "count": 1, "quality": 3, "quest_item": true},
	]
	bus.publish_loot_response(0xDEAD, EventBus.LOOT_OK, 137, items)
	_check("loot response stored (corpse)", bus.loot_corpse() == 0xDEAD)
	_check("loot copper stored", bus.loot_copper() == 137)
	_check("loot items stored (2)", bus.loot_items().size() == 2)
	_check("loot_window_changed emitted", win_events.size() == 1)

	# Taking a slot removes it from the open corpse.
	bus.publish_loot_result({
		"corpse_guid": 0xDEAD, "slot": 1, "status": EventBus.LOOT_OK,
		"item_template_id": 9002, "count": 1, "copper": 0,
	})
	_check("slot take removed slot 1", bus.loot_items().size() == 1 and
		int((bus.loot_items()[0] as Dictionary)["slot"]) == 0)

	# Taking the money clears the pile.
	bus.publish_loot_result({
		"corpse_guid": 0xDEAD, "slot": 0, "status": EventBus.LOOT_OK,
		"item_template_id": 0, "count": 0, "copper": 137,
	})
	_check("money take cleared the pile", bus.loot_copper() == 0)

	# A non-OK take leaves state unchanged + surfaces the reason.
	var last_result := {"v": {}}
	bus.loot_result.connect(func(r): last_result["v"] = r)
	bus.publish_loot_result({
		"corpse_guid": 0xDEAD, "slot": 0, "status": 5, "item_template_id": 0, "count": 0, "copper": 0,
	})
	_check("failed take surfaces status 5", int((last_result["v"] as Dictionary).get("status", -1)) == 5)
	_check("failed take kept 1 item", bus.loot_items().size() == 1)

	# LOOT_CLOSED clears the open corpse.
	var closed := {"v": 0}
	bus.loot_closed.connect(func(c): closed["v"] = c)
	bus.publish_loot_closed(0xDEAD)
	_check("loot closed clears corpse", bus.loot_corpse() == 0)
	_check("loot_closed emitted", int(closed["v"]) == 0xDEAD)

	# A non-OK LOOT_RESPONSE carries no items.
	bus.publish_loot_response(7, 4, 0, [])  # ALREADY_LOOTED
	_check("rejected response has no items", bus.loot_items().is_empty())


# --- Event bus: inventory snapshot -------------------------------------------

func _verify_event_bus_inventory() -> void:
	print("[event_bus/inventory]")
	var bus = EventBus.new()

	_check("inventory unknown before any snapshot", not bus.inventory_known())
	_check("currency unknown before any snapshot", not bus.currency_known())

	var inv_events: Array = []
	var equip_events: Array = []
	bus.inventory_changed.connect(func(m, it, cap): inv_events.append([m, it, cap]))
	bus.equipment_changed.connect(func(eq): equip_events.append(eq))
	var items := [
		{"slot": 0, "item_template_id": 900007, "count": 5, "quality": 1, "binding": 0},
		{"slot": 3, "item_template_id": 900012, "count": 1, "quality": 3, "binding": 1},
	]
	bus.publish_inventory_snapshot(4200, items, 16, [
		{"slot": 13, "item_template_id": 900011, "quality": 2, "binding": 2},
	])
	_check("inventory known after snapshot", bus.inventory_known())
	_check("inventory items stored (2)", bus.inventory_items().size() == 2)
	_check("backpack capacity stored (16)", bus.backpack_slots() == 16)
	_check("inventory_changed emitted", inv_events.size() == 1)
	_check("equipment projection stored", bus.equipment_items().size() == 1)
	_check("equipment_changed emitted", equip_events.size() == 1)
	# The snapshot's money is server-authoritative → currency known from spawn.
	_check("currency known from snapshot money", bus.currency_known())
	_check("snapshot money applied (4200)", bus.copper() == 4200)
	_check("item[1] carries slot/template/quality/binding",
		int((bus.inventory_items()[1] as Dictionary)["slot"]) == 3 and
		int((bus.inventory_items()[1] as Dictionary)["item_template_id"]) == 900012 and
		int((bus.inventory_items()[1] as Dictionary)["quality"]) == 3 and
		int((bus.inventory_items()[1] as Dictionary)["binding"]) == 1)

	# A re-pushed snapshot (after loot/vendor/reward) REPLACES the contents + balance.
	bus.publish_inventory_snapshot(300, [
		{"slot": 0, "item_template_id": 900007, "count": 4, "quality": 1, "binding": 0},
	], 16)
	_check("re-pushed snapshot replaces items (1)", bus.inventory_items().size() == 1)
	_check("re-pushed snapshot updates money (300)", bus.copper() == 300)

	# An empty backpack snapshot is valid (no items, money still authoritative).
	bus.publish_inventory_snapshot(0, [], 16)
	_check("empty snapshot yields no items", bus.inventory_items().is_empty())
	_check("empty snapshot keeps inventory known", bus.inventory_known())


# --- Event bus: vendor + currency --------------------------------------------

func _verify_event_bus_vendor() -> void:
	print("[event_bus/vendor]")
	var bus = EventBus.new()

	_check("currency unknown before any result", not bus.currency_known())

	# A buy applies the server balance.
	var cur_events: Array = []
	bus.currency_changed.connect(func(c, k): cur_events.append([c, k]))
	bus.publish_vendor_buy_result({
		"status": EventBus.RESULT_OK, "vendor_id": 42, "item_template_id": 1201,
		"quantity": 5, "item_guid": 0xABCD, "total_price": 250, "balance": 9750,
	})
	_check("currency known after buy", bus.currency_known())
	_check("balance applied from buy (9750)", bus.copper() == 9750)
	_check("currency_changed emitted", cur_events.size() == 1 and int(cur_events[0][0]) == 9750)

	# A sell applies balance AND pushes a buyback entry.
	var bb_events: Array = []
	bus.vendor_buyback_changed.connect(func(e): bb_events.append(e))
	bus.publish_vendor_sell_result({
		"status": EventBus.RESULT_OK, "backpack_slot": 7, "item_template_id": 1201,
		"quantity": 3, "total_credit": 75, "balance": 9825, "buyback_slot": 0,
	})
	_check("balance applied from sell (9825)", bus.copper() == 9825)
	_check("buyback queue has 1 entry", bus.buyback_entries().size() == 1)
	_check("buyback entry carries item + price",
		int((bus.buyback_entries()[0] as Dictionary)["item_template_id"]) == 1201 and
		int((bus.buyback_entries()[0] as Dictionary)["price"]) == 75)
	_check("vendor_buyback_changed emitted on sell", bb_events.size() == 1)

	# A buyback applies balance AND removes the entry from the ECHOED buyback_slot (#471) —
	# the server tells us which row it repurchased, so no caller-side correlation.
	bus.publish_vendor_buyback_result({
		"status": EventBus.RESULT_OK, "item_template_id": 1201, "quantity": 3,
		"item_guid": 0xBEEF, "price": 75, "balance": 9750, "buyback_slot": 0,
	})
	_check("balance applied from buyback (9750)", bus.copper() == 9750)
	_check("buyback entry removed via echoed slot", bus.buyback_entries().is_empty())

	# A rejected buy still reports its (unchanged) balance + a non-OK result.
	var vr := {"v": {}}
	bus.vendor_result.connect(func(r): vr["v"] = r)
	bus.publish_vendor_buy_result({
		"status": 3, "vendor_id": 42, "item_template_id": 1201, "quantity": 5,
		"item_guid": 0, "total_price": 0, "balance": 9750,
	})
	_check("rejected buy result kind 'buy' status 3",
		String((vr["v"] as Dictionary).get("kind", "")) == "buy" and
		int((vr["v"] as Dictionary).get("status", -1)) == 3)


# --- Event bus: vendor catalog (VENDOR_LIST) ---------------------------------

func _verify_event_bus_vendor_list() -> void:
	print("[event_bus/vendor_list]")
	var bus = EventBus.new()

	var cat_events: Array = []
	bus.vendor_catalog_changed.connect(func(v, it): cat_events.append([v, it]))
	var items := [
		{"item_template_id": 900007, "price": 12, "quality": 1, "stock": -1},
		{"item_template_id": 900012, "price": 250, "quality": 3, "stock": 5},
	]
	bus.publish_vendor_list(990001, items)
	_check("vendor catalog stored (2)", bus.vendor_catalog().size() == 2)
	_check("vendor_catalog_changed emitted", cat_events.size() == 1 and int(cat_events[0][0]) == 990001)
	_check("catalog row carries template/price/stock",
		int((bus.vendor_catalog()[0] as Dictionary)["item_template_id"]) == 900007 and
		int((bus.vendor_catalog()[0] as Dictionary)["price"]) == 12 and
		int((bus.vendor_catalog()[1] as Dictionary)["stock"]) == 5)

	# An empty / unknown vendor catalog is valid (no items).
	bus.publish_vendor_list(7, [])
	_check("empty vendor catalog stored", bus.vendor_catalog().is_empty())


# --- Event bus: trainer ------------------------------------------------------

func _verify_event_bus_trainer() -> void:
	print("[event_bus/trainer]")
	var bus = EventBus.new()

	var list_events: Array = []
	bus.trainer_list_changed.connect(func(n, e): list_events.append([n, e]))
	var entries := [
		{"ability_id": 301, "cost": 50, "required_class": 1, "required_level": 1, "state": EventBus.TRAIN_LEARNABLE},
		{"ability_id": 415, "cost": 9999, "required_class": 1, "required_level": 4, "state": EventBus.TRAIN_CANT_AFFORD},
	]
	bus.publish_trainer_list(64, entries)
	_check("trainer list stored (npc 64)", bus.trainer_npc() == 64)
	_check("trainer entries stored (2)", bus.trainer_entries().size() == 2)
	_check("trainer_list_changed emitted", list_events.size() == 1)
	_check("learnable row not known", not bool((bus.trainer_entries()[0] as Dictionary)["known"]))

	# Learning OK marks the ability known + applies new_balance.
	bus.publish_trainer_learn_result({
		"npc_guid": 64, "ability_id": 301, "status": EventBus.RESULT_OK,
		"cost": 50, "new_balance": 9700,
	})
	_check("balance applied from learn (9700)", bus.copper() == 9700)
	_check("learned ability 301 now known",
		bool((bus.trainer_entries()[0] as Dictionary)["known"]))

	# A rejected learn surfaces its status, unchanged balance.
	var lr := {"v": {}}
	bus.trainer_learn_result.connect(func(r): lr["v"] = r)
	bus.publish_trainer_learn_result({
		"npc_guid": 64, "ability_id": 415, "status": 6, "cost": 0, "new_balance": 9700,
	})
	_check("rejected learn surfaces status 6", int((lr["v"] as Dictionary).get("status", -1)) == 6)


# --- Windows -----------------------------------------------------------------

func _verify_loot_window() -> void:
	print("[loot_window]")
	var bus = EventBus.new()
	var win = LootWindow.new()
	root.add_child(win)
	await _wait(1)
	win.setup(bus)

	var take := {"corpse": 0, "slot": -1, "money": false}
	bus.loot_take_requested.connect(func(c, s, m): take["corpse"] = c; take["slot"] = s; take["money"] = m)
	var released := {"v": 0}
	bus.loot_release_requested.connect(func(c): released["v"] = c)

	bus.publish_loot_response(0xDEAD, EventBus.LOOT_OK, 137, [
		{"slot": 0, "item_template_id": 1201, "count": 2, "quality": 1, "quest_item": false},
		{"slot": 1, "item_template_id": 9002, "count": 1, "quality": 3, "quest_item": true},
	])
	await _wait(1)
	_check("loot window visible on response", win.visible)
	var item_btn := _find_button_containing(win, "Item #9002")
	_check("loot item row rendered", item_btn != null)
	var money_btn := _find_button_containing(win, "Take money")
	_check("loot money button rendered", money_btn != null)
	if item_btn != null:
		item_btn.emit_signal("pressed")
	_check("pressing item emits take intent (slot 1)", int(take["slot"]) == 1 and not bool(take["money"]))
	if money_btn != null:
		money_btn.emit_signal("pressed")
	_check("pressing money emits money take", bool(take["money"]))

	# Close → release intent.
	var close_btn := _find_button_containing(win, "Close")
	if close_btn != null:
		close_btn.emit_signal("pressed")
	_check("pressing close emits release intent", int(released["v"]) == 0xDEAD)
	win.queue_free()


func _verify_trainer_window() -> void:
	print("[trainer_window]")
	var bus = EventBus.new()
	var win = TrainerWindow.new()
	root.add_child(win)
	await _wait(1)
	win.setup(bus)

	var learn := {"npc": 0, "ability": 0}
	bus.trainer_learn_requested.connect(func(n, a): learn["npc"] = n; learn["ability"] = a)

	bus.publish_trainer_list(64, [
		{"ability_id": 301, "cost": 50, "required_class": 1, "required_level": 1, "state": EventBus.TRAIN_LEARNABLE},
		{"ability_id": 415, "cost": 9999, "required_class": 1, "required_level": 4, "state": EventBus.TRAIN_CANT_AFFORD},
	])
	bus.open_trainer(64)
	await _wait(1)
	_check("trainer window visible on open", win.visible)
	var ability_label := _find_label_containing(win, "Ability #301")
	_check("trainer ability row rendered", ability_label != null)
	# A learnable row has a Learn button; press it.
	var learn_btn := _find_button_containing(win, "Learn")
	_check("learn button rendered for learnable row", learn_btn != null)
	if learn_btn != null:
		learn_btn.emit_signal("pressed")
	_check("pressing learn emits intent (npc 64, ability 301)",
		int(learn["npc"]) == 64 and int(learn["ability"]) == 301)
	# The unaffordable row is greyed (no Learn button for #415 — only one Learn button total).
	_check("only one learn button (unaffordable row greyed)",
		_count_buttons_containing(win, "Learn") == 1)
	win.queue_free()


func _verify_vendor_window() -> void:
	print("[vendor_window]")
	var bus = EventBus.new()
	var win = VendorWindow.new()
	root.add_child(win)
	await _wait(1)
	win.setup(bus)

	var buyback := {"slot": -1}
	bus.vendor_buyback_requested.connect(func(s): buyback["slot"] = s)
	var buy := {"vendor": -1, "tmpl": -1, "qty": -1}
	bus.vendor_buy_requested.connect(func(v, t, q): buy["vendor"] = v; buy["tmpl"] = t; buy["qty"] = q)

	# The VENDOR_LIST catalog arrives (auto-pushed on gossip) BEFORE the window opens.
	bus.publish_vendor_list(42, [
		{"item_template_id": 900007, "price": 12, "quality": 1, "stock": -1},
		{"item_template_id": 900012, "price": 250, "quality": 3, "stock": 5},
	])
	bus.open_vendor(42)
	await _wait(1)
	_check("vendor window visible on open", win.visible)

	# The BUY tab renders the real catalog rows (item + server price + stock).
	var buy_btn := _find_button_containing(win, "Item #900012")
	_check("vendor catalog row rendered (real price)", buy_btn != null and
		buy_btn.text.find("2s 50c") != -1 and buy_btn.text.find("stock: 5") != -1)
	if buy_btn != null:
		buy_btn.emit_signal("pressed")
	_check("pressing a catalog row emits buy intent (vendor 42, item 900012)",
		int(buy["vendor"]) == 42 and int(buy["tmpl"]) == 900012 and int(buy["qty"]) == 1)

	# A sell result populates the buyback tab with a repurchase button.
	bus.publish_vendor_sell_result({
		"status": EventBus.RESULT_OK, "backpack_slot": 7, "item_template_id": 1201,
		"quantity": 3, "total_credit": 75, "balance": 9825, "buyback_slot": 0,
	})
	await _wait(1)
	var bb_btn := _find_button_containing(win, "Item #1201")
	_check("buyback entry rendered after sell", bb_btn != null)
	if bb_btn != null:
		bb_btn.emit_signal("pressed")
	_check("pressing buyback emits intent (slot 0)", int(buyback["slot"]) == 0)

	# The balance label reflects the server balance (9825 copper → "98s 25c").
	_check("vendor shows server balance", _find_label_containing(win, "98s 25c") != null)
	win.queue_free()


func _verify_bags_window() -> void:
	print("[bags_window]")
	var bus = EventBus.new()
	var win = BagsWindow.new()
	root.add_child(win)
	await _wait(1)
	win.setup(bus)

	win.toggle()
	await _wait(1)
	_check("bags window visible on toggle", win.visible)
	_check("bags shows unknown money before any snapshot",
		_find_label_containing(win, "unknown") != null)
	_check("bags shows waiting-for-snapshot before any snapshot",
		_find_label_containing(win, "Waiting for inventory") != null)

	# An INVENTORY_SNAPSHOT populates the real item list + money (known from spawn).
	bus.publish_inventory_snapshot(12345, [
		{"slot": 0, "item_template_id": 900007, "count": 5, "quality": 1, "binding": 0},
		{"slot": 3, "item_template_id": 900012, "count": 1, "quality": 3, "binding": 1},
	], 16, [{"slot": 13, "item_template_id": 900011, "quality": 2, "binding": 2}])
	await _wait(1)
	_check("bags shows server money after snapshot",
		_find_label_containing(win, "1g 23s 45c") != null)
	_check("bags renders real item row (#900012)",
		_find_label_containing(win, "Item #900012") != null)
	_check("bags shows a stacked item (x5)",
		_find_label_containing(win, "Item #900007 x5") != null)
	_check("bags shows slot capacity (2 / 16)",
		_find_label_containing(win, "2 / 16") != null)
	_check("bags renders authoritative paperdoll row",
		_find_label_containing(win, "Slot 13 — Item #900011") != null)
	var equipment_intent := {"action": -1, "slot": -1}
	bus.equipment_change_requested.connect(func(a, s):
		equipment_intent["action"] = a; equipment_intent["slot"] = s)
	var equip_button := _find_button_containing(win, "Equip")
	_check("backpack item has keyboard-focusable Equip button", equip_button != null and
		equip_button.focus_mode != Control.FOCUS_NONE)
	_check("Equip button exposes item-specific accessible name", equip_button != null and
		"900007" in equip_button.accessibility_name)
	equip_button.emit_signal("pressed")
	_check("Equip emits backpack slot intent", int(equipment_intent["action"]) == 0 and
		int(equipment_intent["slot"]) == 0)
	bus.publish_equipment_change_result({"status": 6, "action": 0, "slot": 0})
	_check("typed rejection is actionable",
		_find_label_containing(win, "not proficient") != null)
	var unequip_button := _find_button_containing(win, "Unequip")
	_check("Unequip button exposes slot-specific accessible name", unequip_button != null and
		"slot 13" in unequip_button.accessibility_name)
	unequip_button.emit_signal("pressed")
	_check("Unequip emits paperdoll slot intent", int(equipment_intent["action"]) == 1 and
		int(equipment_intent["slot"]) == 13)

	# A re-pushed snapshot (after a purchase) re-renders the grid.
	bus.publish_inventory_snapshot(95, [
		{"slot": 0, "item_template_id": 900007, "count": 4, "quality": 1, "binding": 0},
	], 16)
	await _wait(1)
	_check("bags re-renders after re-pushed snapshot (x4)",
		_find_label_containing(win, "Item #900007 x4") != null)
	_check("bags drops the removed item after re-push",
		_find_label_containing(win, "Item #900012") == null)
	win.queue_free()


# --- Net bridge --------------------------------------------------------------

func _verify_net_bridge() -> void:
	print("[net_bridge]")
	var net := MeridianNetThread.new()
	_check("loot_request frame non-empty", net.build_loot_request_frame(0xDEAD).size() > 0)
	_check("loot_take frame non-empty", net.build_loot_take_frame(0xDEAD, 1, false).size() > 0)
	_check("loot_release frame non-empty", net.build_loot_release_frame(0xDEAD).size() > 0)
	_check("vendor_buy frame non-empty", net.build_vendor_buy_frame(42, 1201, 5).size() > 0)
	_check("vendor_sell frame non-empty", net.build_vendor_sell_frame(42, 7, 3).size() > 0)
	_check("vendor_buyback frame non-empty", net.build_vendor_buyback_frame(0).size() > 0)
	_check("trainer_learn frame non-empty", net.build_trainer_learn_frame(64, 301).size() > 0)
	_check("equipment_change frame non-empty", net.build_equipment_change_frame(0, 3).size() > 0)

	# decode_econ_frame rejects a garbage body safely (kind "") for an econ op, and returns
	# kind "" for a non-econ opcode.
	var bad: Dictionary = net.decode_econ_frame(0x5002, PackedByteArray([0xFF, 0xFF, 0xFF, 0xFF]))
	_check("garbage LOOT_RESPONSE → kind ''", String(bad.get("kind", "x")) == "")
	var bad_inv: Dictionary = net.decode_econ_frame(0x5007, PackedByteArray([0xFF, 0xFF, 0xFF, 0xFF]))
	_check("garbage INVENTORY_SNAPSHOT → kind ''", String(bad_inv.get("kind", "x")) == "")
	var bad_vl: Dictionary = net.decode_econ_frame(0x5107, PackedByteArray([0xFF, 0xFF, 0xFF, 0xFF]))
	_check("garbage VENDOR_LIST → kind ''", String(bad_vl.get("kind", "x")) == "")
	var other: Dictionary = net.decode_econ_frame(0x2001, PackedByteArray())
	_check("non-econ opcode → kind ''", String(other.get("kind", "x")) == "")


# --- helpers -----------------------------------------------------------------

func _find_button_containing(root_node: Node, needle: String) -> Button:
	for n in _walk(root_node):
		if n is Button and (n as Button).text.find(needle) != -1:
			return n
	return null


func _count_buttons_containing(root_node: Node, needle: String) -> int:
	var count := 0
	for n in _walk(root_node):
		if n is Button and (n as Button).text.find(needle) != -1:
			count += 1
	return count


func _find_label_containing(root_node: Node, needle: String) -> Label:
	for n in _walk(root_node):
		if n is Label and (n as Label).text.find(needle) != -1:
			return n
	return null


func _walk(n: Node) -> Array:
	var out: Array = [n]
	for c in n.get_children():
		out.append_array(_walk(c))
	return out
