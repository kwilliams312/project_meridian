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


CHARACTERS_MIGRATIONS = REPO / "server" / "db" / "characters" / "migrations"


def _version_of(up_filename: str) -> str:
    """The leading numeric token of an NNNN_name.up.sql migration (e.g. '0004')."""
    m = re.match(r"^(\d+)_", up_filename)
    assert m, f"migration {up_filename} does not start with an NNNN_ version token"
    return m.group(1)


def _recorded_versions_in_wrapper(wrapper: Path) -> list[str]:
    """Versions the wrapper seeds into schema_migrations (the INSERT VALUES list)."""
    text = wrapper.read_text()
    block = re.search(
        r"INSERT\s+(?:IGNORE\s+)?INTO\s+schema_migrations.*?;",
        text,
        re.IGNORECASE | re.DOTALL,
    )
    if not block:
        return []
    return re.findall(r"\(\s*'([^']+)'\s*\)", block.group(0))


def test_characters_wrapper_seeds_schema_migrations_for_every_up_migration() -> None:
    """#815 single-source-of-truth: the fresh-init wrapper OWNS first-init of the
    schema_migrations tracker, so it must record exactly the versions it SOURCEs.
    If they drift, the deploy-time runner would re-run or skip the wrong migration
    on a persistent realm."""
    wrapper = DB_INIT / "02-characters.sql"
    sourced_versions = sorted(
        _version_of(p.name) for p in CHARACTERS_MIGRATIONS.glob("*.up.sql")
    )
    assert sourced_versions, "no *.up.sql migrations found"
    recorded = sorted(_recorded_versions_in_wrapper(wrapper))
    assert recorded == sourced_versions, (
        "02-characters.sql must seed schema_migrations with every migration version "
        "it SOURCEs (#815).\n"
        f"  versions on disk:      {sourced_versions}\n"
        f"  wrapper records:       {recorded}\n"
        f"  missing from tracker:  {sorted(set(sourced_versions) - set(recorded))}\n"
        f"  stale in tracker:      {sorted(set(recorded) - set(sourced_versions))}"
    )


def test_every_character_migration_declares_an_applied_probe() -> None:
    """#815: the deploy-time runner backfills legacy (pre-tracker) DBs by probing
    each migration's sentinel object. Every *.up.sql must declare its own
    `-- meridian:applied-probe <table:...|column:...>` directive so the backfill
    records exactly what is applied — a migration without one breaks the runner."""
    directive = re.compile(
        r"^--\s*meridian:applied-probe\s+((?:table|column):\S+)", re.MULTILINE
    )
    missing = []
    for up in sorted(CHARACTERS_MIGRATIONS.glob("*.up.sql")):
        if not directive.search(up.read_text()):
            missing.append(up.name)
    assert not missing, (
        "these character migrations lack a `-- meridian:applied-probe "
        "table:<name>|column:<table>.<col>` directive the runner needs (#815): "
        f"{missing}"
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
