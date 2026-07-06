# Server-managed databases

DDL for the databases the **server writes at runtime**. Unlike the world DB
(`schema/sql/world/`), which is an `mcc`-produced, read-only artifact replaced
wholesale each night, these databases are **owned and mutated by the server
processes** — accounts are created, bans are added, session grants are
written/consumed/expired, realm rows heartbeat, the coordinator lease is
CAS-updated. They therefore evolve over time and ship as **reversible, numbered
migrations** rather than a single static DDL dump.

| DB | Owner | This tree covers |
|----|-------|------------------|
| `auth` | `authd` (login, realm list, session grants); `gatewayd` reads/consumes grants + enforces bans; `coordd` heartbeats realm + holds control lease (M3) | `auth/migrations/` — GitHub issue #76 |

> Scope of this deliverable (#76): the **auth DB** only. The characters DB
> (server SAD §4.2) is server-managed too and will get its own
> `server/db/characters/migrations/` tree in a later issue. The world DB is
> **not** here — it lives at `schema/sql/world/` because Tools owns it (IF-4).

Authoritative field list: [`docs/sad/server-sad.md` §4.1](../../docs/sad/server-sad.md)
(auth DB tables), §2.1 (authd), §5.3 (IF-3 session grant), §8.6 (coordinator
lease). This DDL is **co-reviewed with the Server track**.

## Directory layout

```
server/db/
  README.md                         <- this file
  auth/
    migrations/
      0001_init_auth.up.sql         <- account, account_ban, ip_ban, realm, session_grant
      0001_init_auth.down.sql       <- drops them (reverse order)
      0002_realm_control.up.sql     <- realm_control (M3 coordinator lease)
      0002_realm_control.down.sql   <- drops it
```

## Migration convention

- **Numbered pairs.** Every change is a zero-padded, monotonically increasing
  number + a slug, in **two files**: `NNNN_<slug>.up.sql` and
  `NNNN_<slug>.down.sql`. The `up` applies the change; the `down` reverses
  **exactly** what that `up` created (see Backend Standards → Migrations:
  every migration MUST have a working rollback).
- **The sequence is the timeline.** Migrations are applied in ascending number
  order and never modified once merged; a new change is always a new pair. This
  is why `realm_control` (which lands at M3) is `0002`, separate from the M0
  auth base in `0001` — the numbering records *when* each table entered the DB.
- **`down` drops in reverse dependency order** (children before parents) so
  foreign keys never block a rollback, and uses `DROP TABLE IF EXISTS` to stay
  idempotent.
- **Reversibility rule of thumb.** An `up` that only `CREATE`s tables has a
  `down` that only `DROP`s them. A future `up` that adds a column has a `down`
  that drops that column; a data backfill `up` pairs with a `down` that undoes
  or no-ops the backfill. Keep one logical change per migration.

## How migrations are applied

Per server PRD §6 (OPS-01 / D-30), `docker compose up` stands up a full local
realm — `authd`, `worldd`, MariaDB, **and a one-shot `migrations` job**. That
job applies every pending `up` migration in number order against the auth DB
(and, later, the characters DB) before the daemons accept traffic; the nightly
test-realm redeploy (PRD §6) runs the same migrations step. The characters DB
persists across redeploys, so migrations must be **forward-only in production**
— `down` files exist for local/dev rollback and to prove each `up` is
reversible, not for routine production downgrades.

Applying by hand (any MySQL client works — the files are plain DDL):

```sh
# apply, in order
mariadb auth < auth/migrations/0001_init_auth.up.sql
mariadb auth < auth/migrations/0002_realm_control.up.sql

# roll back the most recent, then the base
mariadb auth < auth/migrations/0002_realm_control.down.sql
mariadb auth < auth/migrations/0001_init_auth.down.sql
```

## Dialect

- **MariaDB / MySQL**, `ENGINE=InnoDB`, `DEFAULT CHARSET=utf8mb4
  COLLATE=utf8mb4_unicode_ci` on every table, declared explicitly (baseline
  TD-05), so the DDL is portable regardless of server defaults.
- **Timestamps** are `DATETIME` in UTC. Mutable entities carry `created_at`
  (default `CURRENT_TIMESTAMP`) and `updated_at` (`ON UPDATE
  CURRENT_TIMESTAMP`); append-only rows (`account_ban`, `ip_ban`,
  `session_grant`) carry `created_at` only.
- **Primary keys** are **server-minted**, in contrast to the world DB's
  IF-9-assigned `uint32`s:
  - `account.id`, `account_ban.id`, `ip_ban.id`, `realm.id` are
    `AUTO_INCREMENT` surrogates.
  - **`session_grant.grant_id` is a random `u64` supplied by the app, NOT
    `AUTO_INCREMENT`** — it travels to the client and is presented back at the
    world handshake (§3.1), so it must be unguessable/non-enumerable to prevent
    grant-race attacks. The `session_key` HMAC still gates the proof; the
    random id is defence in depth.
  - `realm_control.realm_id` is a natural 1:1 PK (also the FK to `realm`).
- **Foreign keys** are declared within the auth DB with explicit `ON DELETE`.
  There are **no cross-DB FKs** — the characters DB references `account.id` as a
  *soft* numeric reference only (server SAD §4.2/§4.4).

### FK `ON DELETE` choices

| FK | `ON DELETE` | Why |
|----|-------------|-----|
| `account_ban.account_id → account.id` | `CASCADE` | a ban is meaningless without its account; purging an account purges its ban history |
| `account_ban.banned_by → account.id` | `SET NULL` | keep the ban record if the acting GM's account is later removed; `NULL` reads as "system/automated" |
| `ip_ban.banned_by → account.id` | `SET NULL` | same as above; `ip_ban` has no account FK by design (IP bans are account-independent) |
| `session_grant.account_id → account.id` | `CASCADE` | grants are short-lived and swept; they must not outlive their account |
| `session_grant.realm_id → realm.id` | `CASCADE` | a grant is bound to a realm; removing a realm invalidates its grants |
| `realm_control.realm_id → realm.id` | `CASCADE` | the control/lease row is 1:1 with, and meaningless without, its realm |

## Verification status

Syntax validated with `sqlglot` (MySQL dialect) — all files parse. **Execution
against a live MariaDB is deferred to the MariaDB-service CI job (issue #187);**
MariaDB is not part of the local toolchain, so no `up`/`down` round-trip has
been run against a real server here.
