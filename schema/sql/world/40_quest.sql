-- =====================================================================
-- File 40: quests
-- Mirrors: schema/content/quest.schema.yaml (QST-01)
-- =====================================================================

-- ---------------------------------------------------------------------
-- quest_template
--   Mirrors quest.schema.yaml scalar fields. Flattening:
--     rewards.xp / rewards.money -> reward_xp / reward_money (copper)
--     rewards.items[] / choice_items[] -> quest_reward child (is_choice flag)
--     objectives[]      -> quest_objective child (oneOf by type)
--     prerequisites.quests[] -> quest_prereq child
--     script            -> reserved (M2, type:null in schema) — OMITTED, see README.
-- ---------------------------------------------------------------------
CREATE TABLE quest_template (
  id                 INT UNSIGNED NOT NULL,                 -- IF-9 numeric id
  name               VARCHAR(80)  NOT NULL,                 -- displayName
  summary            VARCHAR(500) NOT NULL,                 -- quest-log text
  offer_text         VARCHAR(2000) NOT NULL,
  completion_text    VARCHAR(2000) NOT NULL,
  level              SMALLINT UNSIGNED NOT NULL,            -- quest level (scalar, not intRange)
  required_level     SMALLINT UNSIGNED NOT NULL DEFAULT 1,
  zone_ref_id        INT UNSIGNED NULL,                     -- zone (zoneRef) -> zone.id
  giver_npc_id       INT UNSIGNED NOT NULL,                 -- giver (npcRef) -> npc_template.id
  turn_in_npc_id     INT UNSIGNED NULL,                     -- turn_in (defaults to giver)

  -- rewards.* scalars (rewards.items/choice_items -> quest_reward)
  reward_xp          INT UNSIGNED NULL,
  reward_money       BIGINT UNSIGNED NULL,                  -- money = copper

  PRIMARY KEY (id),
  CONSTRAINT fk_quest_zone    FOREIGN KEY (zone_ref_id)    REFERENCES zone (id),
  CONSTRAINT fk_quest_giver   FOREIGN KEY (giver_npc_id)   REFERENCES npc_template (id),
  CONSTRAINT fk_quest_turnin  FOREIGN KEY (turn_in_npc_id) REFERENCES npc_template (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- quest_objective — objectives[] (1..4), oneOf discriminated by `type`.
--   kill:    target_npc_id + count
--   collect: item_id + count (drop-gated via loot_entry.quest_ref_id)
--   deliver: item_id + to_npc_id
--   explore: zone_ref_id + poi (string local to zone manifest, WLD-03)
-- ---------------------------------------------------------------------
CREATE TABLE quest_objective (
  quest_id       INT UNSIGNED NOT NULL,                     -- -> quest_template.id
  ordinal        SMALLINT UNSIGNED NOT NULL,                -- array position
  type           ENUM('kill','collect','deliver','explore') NOT NULL,
  target_npc_id  INT UNSIGNED NULL,                         -- kill.target (npcRef)
  item_id        INT UNSIGNED NULL,                         -- collect.item / deliver.item (itemRef)
  to_npc_id      INT UNSIGNED NULL,                         -- deliver.to (npcRef)
  zone_ref_id    INT UNSIGNED NULL,                         -- explore.zone (zoneRef)
  poi            VARCHAR(64)  NULL,                          -- explore.poi (zone-local string id)
  count          SMALLINT UNSIGNED NULL,                    -- kill/collect count (1..200)
  PRIMARY KEY (quest_id, ordinal),
  CONSTRAINT fk_qobj_quest  FOREIGN KEY (quest_id)      REFERENCES quest_template (id),
  CONSTRAINT fk_qobj_target FOREIGN KEY (target_npc_id) REFERENCES npc_template (id),
  CONSTRAINT fk_qobj_item   FOREIGN KEY (item_id)       REFERENCES item_template (id),
  CONSTRAINT fk_qobj_to     FOREIGN KEY (to_npc_id)     REFERENCES npc_template (id),
  CONSTRAINT fk_qobj_zone   FOREIGN KEY (zone_ref_id)   REFERENCES zone (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- quest_prereq — prerequisites.quests[] (all must be complete; QST-02 chains)
-- ---------------------------------------------------------------------
CREATE TABLE quest_prereq (
  quest_id          INT UNSIGNED NOT NULL,                  -- -> quest_template.id
  prereq_quest_id   INT UNSIGNED NOT NULL,                  -- prerequisites.quests[] (questRef)
  PRIMARY KEY (quest_id, prereq_quest_id),
  CONSTRAINT fk_qprereq_quest  FOREIGN KEY (quest_id)        REFERENCES quest_template (id),
  CONSTRAINT fk_qprereq_target FOREIGN KEY (prereq_quest_id) REFERENCES quest_template (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- quest_reward — rewards.items[] (always granted) + rewards.choice_items[]
--   (player picks one). is_choice distinguishes the two arrays.
-- ---------------------------------------------------------------------
CREATE TABLE quest_reward (
  quest_id   INT UNSIGNED NOT NULL,                         -- -> quest_template.id
  is_choice  BOOLEAN NOT NULL,                              -- FALSE=items[], TRUE=choice_items[]
  ordinal    SMALLINT UNSIGNED NOT NULL,                    -- array position within its group
  item_id    INT UNSIGNED NOT NULL,                         -- item (itemRef)
  count      SMALLINT UNSIGNED NOT NULL DEFAULT 1,
  PRIMARY KEY (quest_id, is_choice, ordinal),
  CONSTRAINT fk_qreward_quest FOREIGN KEY (quest_id) REFERENCES quest_template (id),
  CONSTRAINT fk_qreward_item  FOREIGN KEY (item_id)  REFERENCES item_template (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
