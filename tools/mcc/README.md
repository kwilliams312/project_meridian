# `mcc` — Meridian Content Compiler (TLS-01)

C++20 static CLI, zero Godot dependency except resource import. One compiler, two outputs
(TD-06): world-DB SQL (IF-4) for the server + client `.pck` packs (IF-5). Runs on Windows
x64 and Linux x64 from one CMake tree.

**Contract:** [Tools SAD §2](../../docs/sad/tools-sad.md) · **Schemas:** [`/schema/content`](../../schema/content)

Pipeline is a DAG of pure stages: `discover/parse → validate → link → bake → emit-sql / emit-pck`.
CLI (v1): `mcc build [--full|--watch] | check | fmt | diff | pack | install | uninstall | migrate | idmap verify|reassign`.

Determinism is a hard requirement — same source + same `mcc` ⇒ byte-identical SQL and
content-identical `.pck` (double-build hash-compared in CI).
