-- =====================================================================
-- File 60: vendor inventories
-- Mirrors: schema/content/vendor.schema.yaml (ECO-01)
-- Separate from NPCs so multiple NPCs can share an inventory
-- (npc_template.vendor_ref_id -> vendor_inventory.id).
-- =====================================================================

-- ---------------------------------------------------------------------
-- vendor_inventory — the vendor entity (id only; items/buys are children).
--   Named per server-sad §4.3 table family `vendor_inventory`.
-- ---------------------------------------------------------------------
CREATE TABLE vendor_inventory (
  id  INT UNSIGNED NOT NULL,                                -- IF-9 numeric id
  PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- vendor_inventory_item — items[] this vendor sells.
--   limited.* flattened to limited_count / limited_restock_minutes
--   (both NULL = unlimited stock).
-- ---------------------------------------------------------------------
CREATE TABLE vendor_inventory_item (
  vendor_id               INT UNSIGNED NOT NULL,            -- -> vendor_inventory.id
  ordinal                 SMALLINT UNSIGNED NOT NULL,       -- array position
  item_id                 INT UNSIGNED NOT NULL,            -- items[].item (itemRef)
  price_override          BIGINT UNSIGNED NULL,             -- money = copper (else item.price.buy)
  limited_count           INT UNSIGNED NULL,                -- limited.count (NULL = unlimited)
  limited_restock_minutes INT UNSIGNED NULL,                -- limited.restock_minutes
  PRIMARY KEY (vendor_id, ordinal),
  CONSTRAINT fk_vitem_vendor FOREIGN KEY (vendor_id) REFERENCES vendor_inventory (id),
  CONSTRAINT fk_vitem_item   FOREIGN KEY (item_id)   REFERENCES item_template (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- vendor_inventory_buys — buys[]: item classes this vendor purchases.
--   Absent entirely = buys everything sellable (schema default).
-- ---------------------------------------------------------------------
CREATE TABLE vendor_inventory_buys (
  vendor_id   INT UNSIGNED NOT NULL,                        -- -> vendor_inventory.id
  item_class  ENUM('weapon','armor','consumable','quest',
                   'trade_good','container') NOT NULL,      -- buys[] (itemClass)
  PRIMARY KEY (vendor_id, item_class),
  CONSTRAINT fk_vbuys_vendor FOREIGN KEY (vendor_id) REFERENCES vendor_inventory (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
