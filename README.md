# Project Meridian (working title)

An open-source 3D MMORPG in the WoW themepark tradition: tab-target combat, zone-based open world, quests, dungeons, and deep itemization — built **tools-first**, so the same user-accessible creation tools we ship are the ones we use to build the world.

## Platform targets

| Component | Platform | Stack |
|-----------|----------|-------|
| Client | Windows x64 (DX12) + macOS Apple Silicon (Metal) — no RTX required | Godot 4.6+ (Forward+; D3D12 on Windows, native Metal on macOS) — C++ GDExtension + GDScript |
| Tools | Windows x64 | Godot editor plugins ("Forge") + standalone C#/Avalonia data editors ("Codex") |
| Server | Linux (Ubuntu LTS) | C++20, MariaDB — CMaNGOS-inspired architecture, clean-room |

> **Why Godot?** The entire stack — engine, editor, and our tools — is open source and redistributable (Godot is MIT). An earlier draft targeted Unreal Engine 5, but Epic's EULA prevents redistributing the engine or editor plugins, which conflicts with both the open-source goal and shipping Forge to community creators.

## Documents

Start with the baseline — it defines the feature IDs, milestones (M0–M4), and technical decisions every track PRD is synchronized against.

- [Game Design Baseline](docs/00-GAME-DESIGN-BASELINE.md) — shared feature matrix, milestones, cross-track contracts
- [Sync Decisions](docs/01-SYNC-DECISIONS.md) — resolved cross-track conflicts and open action items
- [Server PRD](docs/prd/server-prd.md)
- [Client PRD](docs/prd/client-prd.md)
- [Tools PRD](docs/prd/tools-prd.md) — highest-priority track
- [Art PRD](docs/prd/art-prd.md)
- [Music PRD](docs/prd/music-prd.md)
- [Content Schema v1](schema/content/README.md) — ID grammar, YAML file conventions, per-type schemas; example content in [content/core](content/core), validated by [tools/validate_content.py](tools/validate_content.py)
- [Architecture Overview](docs/02-ARCHITECTURE-OVERVIEW.md) — system context, interface registry (IF-1…IF-9), shared principles
- Software Architecture Documents: [Server](docs/sad/server-sad.md) · [Client](docs/sad/client-sad.md) · [Tools](docs/sad/tools-sad.md) · [Art](docs/sad/art-sad.md) · [Music](docs/sad/music-sad.md)

## How the tracks stay in sync

1. **Shared feature IDs** — every PRD maps the baseline's feature matrix (ACC-01, CMB-01, TLS-02, …) to its own deliverables in a traceability table.
2. **Shared milestones** — M0 Foundation → M1 Greybox Vertical Slice → M2 Systems Depth → M3 Alpha World → M4 Beta/1.0. Every milestone exits through a cross-track integration test (IT-M0…IT-M3) run on the test realm.
3. **Shared contracts** — one network/content schema repo (`/schema`), one content database (`/content`), one asset-ID registry. Tools write content, the compiler produces both server SQL and client paks, so client and server are always testable against the same content.

## Planned repo layout

```
open-mmo/
├── client/     Godot project (+ C++ GDExtension modules)
├── server/     authd + worldd (Linux, C++20)
├── tools/      Forge (Godot editor plugin) + Codex (data editors) + mcc (content compiler)
├── content/    YAML content source of truth
├── schema/     network protocol + content schemas (generated code for all consumers)
└── docs/       baseline + PRDs
```

## Licensing

Code: [Apache-2.0](LICENSE) (clean-room; CMaNGOS is an architectural reference only — no GPL code is copied). Engine: Godot, MIT. Original art/music: CC-BY 4.0. Third-party assets must be CC0/CC-BY — engine-locked marketplace content (Quixel/Fab, Unity Asset Store) is disallowed; AI-generated assets require recorded provenance. No Blizzard assets, names, or code — playstyle inspiration only. See [CONTRIBUTING.md](CONTRIBUTING.md) for the binding clean-room and provenance policies.
