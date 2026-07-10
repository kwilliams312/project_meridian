"""Guard: the docker/deploy DB-init wrappers must apply EVERY schema file.

WHY this guard exists (#479 — "deployed character table missing appearance
column"): `deploy/docker/db-init/*.sql` are the first-boot wrappers the
`meridian-db` image (and the reference compose) run to seed the three schemas.
Each wrapper `CREATE DATABASE` + `SOURCE`s the real schema files. The wrappers
hand-enumerated the files to SOURCE, and `02-characters.sql` was never updated
when migration `0003_appearance_dyes.up.sql` (which adds `character.appearance`)
landed — so DBs seeded via the docker path got a `character` table with NO
appearance column, while local dev (which globs `*.up.sql`, `scripts/dev/_db.sh`)
was fine. worldd's `list_characters` then threw on the missing column and the
CHAR_LIST handler degraded to a silent empty roster.

This test freezes the invariant that the wrappers cover the full schema, so any
future migration/DDL added to `server/db/**/migrations` or `schema/sql/world`
but NOT wired into its db-init wrapper fails loudly HERE (in CI) instead of
silently shipping a half-seeded database to a hosted realm.
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DB_INIT = REPO / "deploy" / "docker" / "db-init"

# Migration-managed schemas: the wrapper must SOURCE every *.up.sql (never the
# *.down.sql rollbacks) from the corresponding migrations directory.
MIGRATION_SCHEMAS = {
    "01-auth.sql": REPO / "server" / "db" / "auth" / "migrations",
    "02-characters.sql": REPO / "server" / "db" / "characters" / "migrations",
}

# The world DB is hand-maintained numbered DDL (not migration-managed): the
# wrapper must SOURCE every numbered `NN_*.sql`, in numeric order.
WORLD_WRAPPER = "03-world.sql"
WORLD_DDL_DIR = REPO / "schema" / "sql" / "world"


def _sourced_files(wrapper: Path) -> list[str]:
    """Return the basenames the wrapper SOURCEs, in wrapper order."""
    text = wrapper.read_text()
    # `SOURCE /schemas/<db>/<file>.sql;` — capture the trailing basename.
    return re.findall(r"^\s*SOURCE\s+\S*/([^/;\s]+\.sql)\s*;", text, re.MULTILINE)


def test_migration_wrappers_source_every_up_migration() -> None:
    for wrapper_name, migrations_dir in MIGRATION_SCHEMAS.items():
        wrapper = DB_INIT / wrapper_name
        assert wrapper.is_file(), f"missing db-init wrapper {wrapper_name}"
        expected = sorted(p.name for p in migrations_dir.glob("*.up.sql"))
        assert expected, f"no *.up.sql migrations found in {migrations_dir}"
        sourced = _sourced_files(wrapper)
        assert sorted(sourced) == expected, (
            f"{wrapper_name} does not SOURCE every migration.\n"
            f"  migrations on disk: {expected}\n"
            f"  wrapper SOURCEs:    {sorted(sourced)}\n"
            f"  missing:            {sorted(set(expected) - set(sourced))}\n"
            f"  stale:              {sorted(set(sourced) - set(expected))}"
        )


def test_characters_wrapper_applies_the_appearance_migration() -> None:
    """Regression pin for #479: the exact migration that was dropped."""
    sourced = _sourced_files(DB_INIT / "02-characters.sql")
    assert "0003_appearance_dyes.up.sql" in sourced, (
        "02-characters.sql must SOURCE 0003_appearance_dyes.up.sql — the "
        "migration that adds character.appearance (#479 root cause)"
    )


def test_world_wrapper_sources_every_ddl_file_in_numeric_order() -> None:
    expected = sorted(p.name for p in WORLD_DDL_DIR.glob("[0-9]*.sql"))
    assert expected, f"no numbered DDL found in {WORLD_DDL_DIR}"
    sourced = _sourced_files(DB_INIT / WORLD_WRAPPER)
    assert sourced == expected, (
        f"{WORLD_WRAPPER} must SOURCE every world DDL file in numeric order.\n"
        f"  DDL on disk:     {expected}\n"
        f"  wrapper SOURCEs: {sourced}"
    )
