<!-- SPDX-License-Identifier: Apache-2.0 -->
# Nightly test-realm redeploy + smoke + rollback (#94)

The nightly pipeline that keeps the IT-M0 **test realm** (docs/it-m0-runbook.md
§3) tracking `main`: it redeploys the latest published server images, smoke-tests
them, and rolls back if the smoke fails. Implements **D-30 §10 rule 5** ("the
nightly test realm tracks `main`") and issue #94.

- **Script:** [`deploy/scripts/redeploy.sh`](../../deploy/scripts/redeploy.sh) — the deploy → smoke → rollback logic.
- **Workflow:** [`.github/workflows/nightly-redeploy.yml`](../../.github/workflows/nightly-redeploy.yml) — the nightly `schedule:` + manual `workflow_dispatch`.

Feeds off / complements: **#175** (GHCR autopublish of `authd`/`worldd` — the
images this deploys), **#177** (the reference compose stack it runs on the host),
**#159** (the test-realm host it targets), **#111** (the headless bot that drives
the login→enter-world smoke).

## What it does each night

1. **Record** the currently-running image tag on the host (the rollback target).
2. **Deploy** the target tag (`latest` by default) via `docker compose pull` +
   `up -d` in the host's compose dir.
3. **Smoke** the new deployment:
   - **L1 (always):** both daemons report `running` (the image HEALTHCHECK is the
     daemon's own `--version`), and a **TLS 1.3 handshake** reaches `authd:7100`
     (IF-1) and `worldd:7200` (IF-2). Proves "the daemons come up."
   - **L2 (if `BOT_BIN` configured):** the headless `meridian-bot` (#111) drives
     **login → enter-world → move** and we assert its `BOT_RESULT` line
     (`handshake_ok=1` and `moves_accepted>0`). Proves "a basic login/enter-world
     works." Skipped with a notice if no bot binary is configured.
4. **On smoke failure → ROLL BACK** to the recorded previous tag and **fail the
   job loudly** (non-zero exit → red run → GitHub pages the team).

Idempotent: re-running with the same tag is a no-op; a failed run always leaves
the realm on the previous good tag, never half-deployed.

## Skip-when-unconfigured guard

The test-realm host (#159) is **not provisioned yet**. To avoid a nightly red-X
before it exists, the `preflight` job checks whether the `DEPLOY_TARGET` secret is
set. If it is **unset**, the deploy job is **skipped** and the run ends **green**
with a `::notice::`. Once the host + secrets below are configured, the deploy
runs automatically — **no workflow edit needed**.

## Required secrets / variables (configure at go-live)

Set under **repo Settings → Secrets and variables → Actions**. The `redeploy`
job runs in the `test-realm` **Environment** — put the secrets there (and add any
required reviewers / branch filters) for an extra approval gate if desired.

| Name | Kind | Required | Purpose |
|------|------|----------|---------|
| `DEPLOY_TARGET` | secret | **yes (the gate)** | SSH target of the host, e.g. `deploy@realm.example.com`. **Presence enables the pipeline.** |
| `DEPLOY_SSH_KEY` | secret | yes | PEM private key for the deploy user (least-privilege: docker + the compose dir only). |
| `DEPLOY_KNOWN_HOSTS` | secret | recommended | The host's SSH public key line(s) — pins host identity, avoids blind TOFU. |
| `MERIDIAN_TAG` | var | no | Image tag to deploy. Default `latest` (tracks `main`). |
| `COMPOSE_DIR` | var | no | Compose dir on the host. Default `/opt/meridian/deploy/docker`. |
| `BOT_BIN` | var | no | Path to `meridian-bot` on the host — enables the L2 login→enter→move smoke. |
| `BOT_USER` / `BOT_PASS` | secret | only if `BOT_BIN` set | Pre-seeded account creds (runbook §4.1) the L2 bot logs in with. |

## Go-live steps (when #159's host exists)

1. **Provision the host (#159):** Ubuntu LTS + Docker, firewall opening only
   7100/7200, a `deploy` user in the `docker` group.
2. **Stage the compose stack on the host:** copy `deploy/docker/`
   (`docker-compose.yml`, `db-init/`, and a real `certs/server.{crt,key}`) to
   `COMPOSE_DIR` (default `/opt/meridian/deploy/docker`). Verify a manual
   `MERIDIAN_TAG=latest docker compose up -d` brings the realm up.
3. **Seed a smoke account** (for L2) via `meridian-account create --username <bot>`
   in the auth DB (runbook §4.1); note the password for `BOT_PASS`.
4. **Add the secrets/vars** from the table above. Setting `DEPLOY_TARGET` is what
   flips the pipeline on.
5. **Dry-run once** manually: Actions → *nightly-redeploy* → *Run workflow*. Watch
   it deploy → smoke → (report). Confirm green.
6. Nightly cron (`17 7 * * *` UTC) takes over.

## Verify the logic locally (no host, no Docker)

```bash
bash -n     deploy/scripts/redeploy.sh                        # syntax
shellcheck  deploy/scripts/redeploy.sh                        # lint
deploy/scripts/redeploy.sh --dry-run --simulate-smoke=pass    # deploy path
deploy/scripts/redeploy.sh --dry-run --simulate-smoke=fail    # rollback path (exits 1)
```

`--dry-run` prints every remote command instead of running it and forces the
smoke outcome via `--simulate-smoke`, so both the deploy and the rollback
decision branches are exercisable with no infrastructure. A real rehearsal on a
Docker box uses `--local` (runs against `deploy/docker/docker-compose.yml`).
