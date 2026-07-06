# `/client` — Meridian game client

Godot 4.6+ Forward+ game client for **Windows x64 (D3D12)** and **macOS Apple Silicon
(Metal)** (TD-01/TD-02, D-28). Hot path in C++ GDExtension; UI/flow in GDScript behind
an MVVM boundary. The client is a *predictor and presenter* — server is law.

**Read with:** [Client PRD](../docs/prd/client-prd.md) · [Client SAD](../docs/sad/client-sad.md)

## GDExtension modules (hot path, C++)

`net` (sockets, TLS, FlatBuffers codec, connection FSM) · `sim` (entity mirror,
prediction/reconciliation, interpolation) · `stream` (chunk streamer IF-6, pack mount
IF-5) · `datastore` (compiled content lookup).

## GDScript (interaction path)

Scene flow (Boot/Login/CharSelect/World) · entity presentation · typed event bus ·
ViewModels + HUD Control views · audio runtime hooks · GM UI · (M3) Lua addon host.

## Layout (filled in by the M0 skeleton work)

```
client/
  project.godot         Godot project (engine pinned per milestone, TD-01)
  gdextension/          net / sim / stream / datastore (C++ + godot-cpp)
  scenes/  ui/          GDScript scene tree + HUD
  art/                  imported resources (res://), LFS
```
