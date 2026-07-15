"""Guard: the world-content-seed Job must WHOLESALE-refresh the world schema.

WHY (#814 — "persistent realm serves empty world tables after a content column
lands"): `deploy/helm/meridian/templates/world-content-seed.yaml` used to reseed
by TRUNCATE-ing every `meridian_world` table then loading the baked content DML.
On a persistent PVC that froze the schema at its first-`initdb` state, so when
content evolved the schema (#698 added `world_manifest.compatibility_version`)
the new DML failed with `ERROR 1054 Unknown column`, the #492 guard swallowed it
to exit 0, and the realm was left with EMPTY world tables.

The fix does a wholesale schema refresh: DROP + recreate `meridian_world` from
the image's baked DDL wrapper (`03-world.sql`), then load the content DML — so
the schema always matches the shipped build. This test freezes that contract so
a future edit can't silently regress to TRUNCATE (and can't duplicate the world
DDL SOURCE list, which must stay owned solely by the CI-gated `03-world.sql`).
"""

import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parent.parent
SEED = REPO / "deploy" / "helm" / "meridian" / "templates" / "world-content-seed.yaml"

pytestmark = pytest.mark.unit


def _seed_text() -> str:
    assert SEED.is_file(), f"missing seed template {SEED}"
    return SEED.read_text()


def test_seed_drops_and_recreates_the_world_db_from_the_baked_wrapper() -> None:
    """Reseed must DROP the world DB and recreate it via the baked DDL wrapper."""
    text = _seed_text()
    assert "DROP DATABASE IF EXISTS" in text, (
        "seed must DROP the world DB on reseed (wholesale schema refresh, #814)"
    )
    # Reuse the image's baked wrapper — the single source of truth for the world
    # DDL SOURCE list (CI-gated by test_db_init_migration_coverage.py).
    assert "/docker-entrypoint-initdb.d/03-world.sql" in text, (
        "seed must recreate the world schema by piping the baked DDL wrapper "
        "03-world.sql, not by re-implementing it"
    )


def test_seed_loads_content_dml_after_recreating_the_schema() -> None:
    text = _seed_text()
    # The content DML load must still happen (into $DB_NAME from $SQL_PATH), and
    # it must come AFTER the schema is recreated from the wrapper.
    assert re.search(r'mysql\s+"\$DB_NAME"\s*<\s*"\$SQL_PATH"', text), (
        "seed must load the baked content DML ($SQL_PATH) into $DB_NAME"
    )
    # Anchor on the actual command lines (prose comments mention these tokens
    # too, so match the executable statements, not the surrounding docs).
    drop_at = text.index('mysql -e "DROP DATABASE IF EXISTS')
    wrapper_at = text.index("mysql < /docker-entrypoint-initdb.d/03-world.sql")
    content_at = text.index('mysql "$DB_NAME" < "$SQL_PATH"')
    assert drop_at < wrapper_at < content_at, (
        "order must be DROP -> recreate-from-wrapper -> load content DML"
    )


def test_seed_no_longer_truncates() -> None:
    """Regression pin: the TRUNCATE-only reseed path must be gone (#814)."""
    text = _seed_text()
    assert "TRUNCATE TABLE" not in text, (
        "seed must NOT issue TRUNCATE TABLE — a TRUNCATE-only reseed leaves a "
        "persistent DB frozen at its stale first-initdb schema (the #814 root cause)"
    )
    assert "for t in $TABLES" not in text, (
        "the old per-table TRUNCATE loop must be gone"
    )


def test_seed_does_not_duplicate_the_world_ddl_source_list() -> None:
    """The world DDL SOURCE list lives only in 03-world.sql (CI-gated)."""
    # No literal `SOURCE /schemas/world/NN_*.sql` lines in the Job — it must
    # delegate to the baked wrapper so the two can't drift.
    assert not re.search(r"SOURCE\s+/schemas/world/", _seed_text()), (
        "seed must not inline the world DDL SOURCE list; pipe 03-world.sql "
        "(the single, CI-gated source of truth) instead"
    )


def test_seed_only_touches_the_world_db_never_user_data() -> None:
    """DROP must target only the world DB, never the auth/characters user DBs."""
    text = _seed_text()
    for db in ("meridian_auth", "meridian_characters"):
        assert not re.search(rf"DROP\s+DATABASE[^\n;]*{db}", text), (
            f"seed must never DROP {db} — those hold user data (#814)"
        )
    # The only DROP DATABASE targets the world DB var ($DB_NAME).
    for m in re.finditer(r"DROP\s+DATABASE\s+IF\s+EXISTS\s+([^\n;]+)", text):
        target = m.group(1)
        assert "$DB_NAME" in target, (
            f"DROP DATABASE target must be the world DB var $DB_NAME, got: {target!r}"
        )


def test_seed_preserves_the_492_failsafe_and_hash_gate() -> None:
    """The #492 non-zero->exit-0 trap and the content-hash skip must remain."""
    text = _seed_text()
    assert "exit 0" in text and "trap " in text, "must keep the #492 fail-safe trap"
    assert "content_hash" in text, "must keep the content-hash idempotency gate"
