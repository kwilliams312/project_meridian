# `/client` — Meridian game client

Godot 4.7 Forward+ game client for **Windows x64 (D3D12)** and **macOS Apple Silicon
(Metal)** (TD-01/TD-02, D-28). Hot path in C++ GDExtension; UI/flow in GDScript behind
an MVVM boundary. The client is a *predictor and presenter* — server is law.

**Read with:** [Client PRD](../docs/prd/client-prd.md) · [Client SAD](../docs/sad/client-sad.md)

## GDExtension modules (hot path, C++)

`net` (sockets, TLS, FlatBuffers codec, connection FSM) · `sim` (entity mirror,
prediction/reconciliation, interpolation) · `stream` (chunk streamer IF-6, pack mount
IF-5) · `datastore` (compiled content lookup).

Bootstrap status (#158): a placeholder `MeridianClient` module compiles/links against
the pinned godot-cpp to prove the toolchain. The real modules land in later issues
(e.g. the C++ movement controller, #102).

`MeridianTelemetry` (#168, D-29) — the client half of the telemetry triple's
ERROR/CRITICAL log channel: a thin GDExtension binding over an **engine-free** capture
core (`telemetry_log_*`) that captures **only ERROR/CRITICAL** events, attaches
session/build/platform context (**no PII**, privacy §3), batches + rate-limits them,
serializes to a Sentry-compatible envelope, and ships to a **configurable** endpoint
(the #167 ingest; a no-op sink when unset). Honors the opt-out toggle (privacy §5).
The policy lives in the engine-free core and is unit-tested without Godot; see
`gdextension/meridian/test/telemetry_log_capture_test.cpp`.

```gdscript
# Autoload wiring (illustrative): route Godot's logger + module logs into the
# client telemetry channel and drain it off the game loop.
var tel := MeridianTelemetry.new()
tel.configure(session_id, build_version, "macos-arm64")   # no PII — ephemeral session id
tel.set_endpoint(project_ingest_url)                       # empty => no-op/local sink
tel.set_enabled(Settings.telemetry_opt_in)                 # opt-out toggle (privacy §5)
# For each captured log line (ERROR/CRITICAL only actually ship):
tel.capture_log(MeridianTelemetry.SEVERITY_ERROR, msg, "net")
# On a low-frequency timer (never on the frame path):
tel.poll()                                                 # flush + ship a due batch
```

## GDScript (interaction path)

Scene flow (Boot/Login/CharSelect/World) · entity presentation · typed event bus ·
ViewModels + HUD Control views · audio runtime hooks · GM UI · (M3) Lua addon host.

## Layout

```
client/
  ENGINE_VERSION            Pinned Godot + godot-cpp versions/hashes (machine-readable)
  godot-cpp/                Vendored bindings — git submodule, pinned (see below)
  gdextension/meridian/src/ Placeholder MeridianClient module (bootstrap #158)
  CMakeLists.txt            GDExtension build — CMake path (CI-friendly, no SCons)
  SConstruct                GDExtension build — canonical godot-cpp SCons path
  project/                  Godot project
    project.godot           Forward+ (D3D12 / Metal per TD-02, D-28)
    meridian.gdextension    Extension manifest (win-x64 + macos-arm64, one file per D-28)
    bin/                    Compiled libmeridian.* dropped here (gitignored artifacts)
  .godot-bin/               Pinned engine binaries fetched by scripts/fetch-deps.sh (gitignored)
```

Later milestone dirs (`scenes/`, `ui/`, `art/`) are added by the M0 skeleton work.

---

## Pinned toolchain

Per Client SAD §9 R4 / PRD §12 risk 4, the engine is **pinned per milestone** and
godot-cpp is **vendored** at the commit whose `extension_api.json` matches the engine
exactly. Upgrade only at milestone boundaries. The machine-readable copy of this table
lives in [`ENGINE_VERSION`](./ENGINE_VERSION); keep the two in sync.

### Godot engine — `4.7-stable`

Owner-decided bump 4.6 → 4.7 (issue #262): 4.7 matches the local install and godot-cpp's
mainline (there is no 4.6 release branch/tag upstream — the prior pin was a bare `master`
commit). See `../docs/01-SYNC-DECISIONS.md`. Renderer: Forward+ on **D3D12 (Windows x64)**
and native **Metal (macOS Apple Silicon)** per TD-02 / D-28; Vulkan/MoltenVK stays
buildable as a diagnostic fallback only.

| Asset | Filename | SHA-512 |
|---|---|---|
| Windows x64 editor | `Godot_v4.7-stable_win64.exe.zip` | `41645a90…ba0522b9` |
| macOS universal editor | `Godot_v4.7-stable_macos.universal.zip` | `0d5d635e…b8f79229` |
| Export templates (both platforms) | `Godot_v4.7-stable_export_templates.tpz` | `1035dfde…138c1120e` |

Full 128-char sums are in [`ENGINE_VERSION`](./ENGINE_VERSION). Downloads come from
`https://github.com/godotengine/godot/releases/download/4.7-stable/`.

> **Note (`.universal` on macOS):** the pinned macOS *editor* zip is universal (Intel +
> Apple Silicon) because that is the only editor build the Godot project ships. The
> *client we build and ship* is arm64-only per SAD §9.6 — the universal editor just runs
> the toolchain on a dev Mac. Apple Silicon is the only supported client target (no Intel
> Macs, D-28 rule 2).

### godot-cpp — commit `5ffd70e34d0ab87009a9f0ffa3361bc8f4b09731`

Vendored as a **git submodule** at `client/godot-cpp`.

This is the godot-cpp `master` commit *"gdextension: Sync with upstream commit
`5b4e0cb…` (4.7-stable)"* — its `extension_api.json` reads **`Godot Engine
v4.7.stable.official`**, the exact match to the pinned engine (that `5b4e0cb…` upstream
hash is the engine's own `4.7-stable` tag, and the local editor reports
`4.7.stable.official.5b4e0cb0f`).

**Why a commit and not a branch/tag:** godot-cpp has **no `4.7` branch and no
`godot-4.7-stable` tag** upstream yet — the newest release tag is `godot-4.5-stable`, and
`master` carries the 4.7 `extension_api`. This per-commit pin is the exact `master` sync
commit for 4.7 (the direct analog of the prior 4.6 pin) and the only ref that is exactly
4.7.

---

## Obtaining the pinned engine (don't commit binaries)

Multi-GB editor/template binaries are **never committed**. Fetch + verify them:

```bash
# From repo root — inits the godot-cpp submodule at its pin AND downloads +
# SHA-512-verifies the host-platform editor + export templates into
# client/.godot-bin/ (gitignored).
scripts/fetch-deps.sh

# Just the submodule (e.g. CI that supplies the engine another way):
scripts/fetch-deps.sh --submodule-only
```

Or manually: download the assets in the pin table from the release page above, verify
against the sums in `ENGINE_VERSION`, and install the export templates via
`Editor → Manage Export Templates → Install from File` (the `.tpz`).

---

## Building the GDExtension

First get the vendored bindings (once):

```bash
git submodule update --init --recursive   # or: scripts/fetch-deps.sh --submodule-only
```

The compiled library is dropped into `client/project/bin/` where
`project/meridian.gdextension` expects it. Two equivalent build paths:

### CMake (CI-friendly, no SCons dependency)

```bash
cd client
cmake -B build -DGODOTCPP_TARGET=template_debug     # or template_release / editor
cmake --build build -j
```

### SCons (canonical godot-cpp path)

```bash
cd client
# Windows x64:
scons platform=windows target=template_debug
# macOS Apple Silicon:
scons platform=macos arch=arm64 target=template_debug
```

### Windows x64 notes
- Toolchain: MSVC (VS 2022 Build Tools) or MinGW; Python 3 + `pip install SCons` for the
  SCons path, or CMake ≥ 3.22 for the CMake path.
- Output: `client/project/bin/libmeridian.windows.<target>.x86_64.dll`.

### macOS Apple-Silicon notes
- Toolchain: Xcode command-line tools (`clang++`); CMake ≥ 3.22 or `pip install SCons`.
- Build arm64 only (`arch=arm64` / native host) — **no universal2** per SAD §9.6.
- Output: `client/project/bin/libmeridian.macos.<target>.framework/`.

### Verifying the extension loads

```bash
# From client/, with the pinned editor on PATH as `godot`:
godot --headless --path project \
  --eval 'print(MeridianClient.new().get_version())'
# → meridian-client 0.0.1 (godot 4.7-stable)
```

---

## CI

The first client-side CI job lands in [`.github/workflows/client.yml`](../.github/workflows/client.yml)
(#112 / A-16). It runs on the self-hosted Apple-Silicon runner (`runs-on: [self-hosted,
macOS]`). Two runner quirks are handled explicitly: the box registers with the `macOS`
label set (no `arm64` label), and its Actions runner **runs under Rosetta 2** so
`uname -m` reports `x86_64` inside a step. So the job never gates on `uname -m` — it
detects Apple Silicon Rosetta-proof via `sysctl -n hw.optional.arm64` (== 1 even from an
x86_64 process), invokes the toolchain natively with `arch -arm64 cmake/ctest`, passes
`-DCMAKE_OSX_ARCHITECTURES=arm64`, and asserts every produced binary is `arm64` via
`lipo` so a silent x86_64 build can't pass. Two jobs:

- **`client-cores`** — builds the engine-agnostic cores with
  `-DMERIDIAN_CLIENT_NET=ON -DMERIDIAN_BOT=ON -DMERIDIAN_CLIENT_TESTS=ON` and runs their
  ctest suites (the `clientnet-test` 77 checks, the bot world-session/FSM tests, and the
  GDExtension core tests). This verifies **OpenSSL** (TLS 1.3 + SRP-6a +
  ChaCha20-Poly1305/HKDF — not mbedTLS) and the FlatBuffers codec compile clean on arm64.
- **`client-gdext`** — checks out the pinned godot-cpp submodule and builds the
  GDExtension into `client/project/bin/libmeridian.macos.template_debug.framework`, then
  asserts the framework binary is arm64 and links OpenSSL. godot-cpp's compiled bindings
  are cached (keyed on `ENGINE_VERSION`).

Godot **export** for win-x64 + macos-arm64 (D-28 phase 1 build-health smoke) remains a
later follow-up on top of this build job.
