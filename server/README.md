# `/server` — Meridian game server

The authoritative Linux C++20 server container group. **Clean-room** implementation
under Apache-2.0 — CMaNGOS/TrinityCore are architectural references only, no GPL code
is copied (see [CONTRIBUTING.md](../CONTRIBUTING.md)).

**Read with:** [Server PRD](../docs/prd/server-prd.md) · [Server SAD](../docs/sad/server-sad.md)

## Processes (phased per D-23 / server SAD §1)

| Process | Role | Lands |
|---------|------|-------|
| `authd` | login, realm list, session grants (stateless, LB-able) | M0 |
| `worldd` | shard worker: map simulation, no client listener | M0 (recast M3) |
| `gatewayd` | owns client TCP + IF-2 AEAD; routes to workers | M2 |
| `servicesd` | realm-global services: chat, guilds, mail, AH, LFG | M2 |
| `coordd` | realm coordinator: shard lifecycle, placement, transfers | M3 |

## Shared libraries

`libmeridian-core` (logging, config, threading, RNG) · `libmeridian-db` (async MariaDB) ·
`libmeridian-proto` (generated from `/schema`, D-01) · `libmeridian-game` (simulation systems).

## Databases (MariaDB, 3-DB split)

`auth` · `characters` (only durable player state) · `world` (read-only `mcc` artifact, IF-4).

## Layout (filled in by the M0 skeleton work)

```
server/
  CMakeLists.txt        build root (C++20, CMake)
  libs/                 core / db / proto / game
  authd/  worldd/       daemon entry points
  ops/                  Dockerfiles, compose, grafana/, helm/ (D-30)
  test/                 unit + sim harness + load profiles
```
