-- =====================================================================
-- File 90: gossip + M2 stubs
-- Mirrors: schema/content/npc.schema.yaml interaction.gossip_text (single
-- page per npc for v1; gossip graphs are an M2 schema).
-- =====================================================================

-- ---------------------------------------------------------------------
-- gossip — one page of gossip text per NPC (interaction.gossip_text).
--   Split from npc_template so it is in the IF-7 hot-reload whitelist as
--   its own `gossip` delta class (server-sad §5.4.3) and so npc_template
--   rows stay compact. 1:1 with the owning NPC.
-- ---------------------------------------------------------------------
CREATE TABLE gossip (
  npc_id  INT UNSIGNED NOT NULL,                            -- -> npc_template.id (1:1)
  `text`  VARCHAR(2000) NOT NULL,                           -- interaction.gossip_text (single page)
  PRIMARY KEY (npc_id),
  CONSTRAINT fk_gossip_npc FOREIGN KEY (npc_id) REFERENCES npc_template (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------
-- class_level_stats — M2 (server-sad §4.3). STUB.
--   No content schema exists yet (statprofile refs arrive M2, per
--   npc.schema.yaml). Table declared here as a commented stub so the
--   family is reserved; enable when the M2 stat-profile schema lands.
-- ---------------------------------------------------------------------
-- CREATE TABLE class_level_stats (
--   class_id    INT UNSIGNED NOT NULL,   -- IF-9 numeric class id (M2 schema)
--   level       SMALLINT UNSIGNED NOT NULL,
--   health      INT UNSIGNED NOT NULL,
--   mana        INT UNSIGNED NULL,
--   strength    INT UNSIGNED NOT NULL,
--   agility     INT UNSIGNED NOT NULL,
--   stamina     INT UNSIGNED NOT NULL,
--   intellect   INT UNSIGNED NOT NULL,
--   spirit      INT UNSIGNED NOT NULL,
--   PRIMARY KEY (class_id, level)
-- ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- Restore FK enforcement for any post-load integrity checks.
SET FOREIGN_KEY_CHECKS = 1;
