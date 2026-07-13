<!-- SPDX-License-Identifier: Apache-2.0 -->
# Character-Select Redesign — Design Spec (#639)

**Epic:** #637 (character-select & login UX redesign)
**Story:** #639
**Date:** 2026-07-13
**Status:** Draft — awaiting user review

## Goal

Split the current single-screen char-select (which crams the roster list *and*
the full create form side by side) into two focused views:

1. **Roster view** — the character list, the selected character's paperdoll, and
   three actions: **Delete**, **Create New Character**, **Enter Realm**.
2. **Creation view** — a full-screen character-creation screen with a large,
   drag-to-rotate paperdoll, race / class / customization pickers, and a name
   field centered at the bottom.

This is a **client-only UI restructure**. It preserves the existing net flow
(CharList / CharCreate / CharDelete / EnterWorld), the appearance data contract,
and the #629 create-result status mapping.

## Decisions (locked with product owner)

| Decision | Choice |
|----------|--------|
| Creation UI container | **In-scene full-screen view** — the roster view is replaced by the creation view, which returns to the roster on create/cancel. Not a separate OS window, not a modal overlay. Inherits the #630 `canvas_items` window scaling for free. |
| Creation-view paperdoll | **Drag-to-rotate**, live-updating as race/class/customization change. |
| Selection controls | **Keep the existing `OptionButton` dropdowns** (race, class, hair, face, skin), laid out larger. Upgradeable later. |
| "Enter World" button | **Relabel to "Enter Realm"** on the roster view (the enter-world net intent is unchanged; only the button text changes). |

## Architecture

Three well-bounded units, communicating through Godot signals:

```
CharSelect (scenes/charselect/char_select.tscn + .gd)   ← existing, becomes the CONTROLLER + roster view
  ├─ RosterView (in-scene)                               list + selected paperdoll + Delete/Create/Enter Realm
  └─ CharacterCreate (scenes/charselect/character_create.tscn + .gd)  ← NEW full-screen creation view
        emits: character_confirmed(name, race, class, appearance)
               creation_cancelled()
```

- **`char_select.gd` stays the controller.** It owns the live net session, the
  cached roster, and view switching. It shows the roster by default; on **Create
  New Character** it instances/reveals `CharacterCreate` full-rect and hides the
  roster; on the create view's `character_confirmed`/`creation_cancelled` signals
  it returns to the roster. On confirm it drives the existing
  `build_char_create_request_frame(...)` and, on the OK result, re-lists and
  selects the new character (the existing `_pending_select_id` path).
- **`CharacterCreate` is a self-contained view.** Inputs: the appearance catalog
  + roster metadata it needs to populate pickers (reuse `character_appearance.gd`
  and the existing race/class sources). Outputs: the two signals above. It does
  **not** touch the net layer — it hands the chosen values back to the controller,
  which owns all networking. This keeps the create view unit-testable without a
  session.
- **Paperdoll widget.** The current in-code preview (`_build_placeholder_preview`:
  `SubViewportContainer` + `SubViewport` + `Camera3D` + light) is factored into a
  small reusable builder so both the roster view (selected character) and the
  creation view (large, drag-to-rotate) use the same code. Drag-to-rotate = mouse
  drag on the `SubViewportContainer` yaws the preview root; live refresh reuses the
  existing `_refresh_preview(...)` path.

### Roster view layout
- Left/main: the character **list** (`ItemList`, existing).
- A **paperdoll** of the currently-selected character (front-facing; may reuse the
  shared paperdoll widget — rotation on the roster is optional, not required).
- Action row: **Delete**, **Create New Character**, **Enter Realm**.
  - Delete / Enter Realm are enabled only when a character is selected (existing
    guard behavior).
  - Empty roster: the list is empty, actions requiring a selection are disabled,
    and the status line prompts creation (existing copy). **Create New Character**
    is always enabled. We do **not** auto-open the creation view on an empty
    roster (keep entry explicit).

### Creation view layout
- **Large drag-to-rotate paperdoll**, prominent (center/left).
- **Race** dropdown, **Class** dropdown, **Customization**: **Hair / Face / Skin**
  dropdowns (the existing appearance pickers), grouped clearly.
- **Name** field (`LineEdit`, max_length 32) **centered at the bottom**.
- Actions: **Create** (confirm) and **Cancel/Back** (return to roster, no-op).
- The paperdoll live-updates as race/class/hair/face/skin change (existing
  `_refresh_preview_from_form` behavior, moved into this view).

## Data flow (unchanged contracts)

1. Controller has the live session (from login) and the cached roster.
2. **Create New Character** → controller shows `CharacterCreate`.
3. User picks values → `character_confirmed(name, race, class, appearance)`.
4. Controller validates locally (non-empty name ≤ 32; race/class from the frozen
   roster — same as today) and sends `CHAR_CREATE_REQUEST`.
5. `CHAR_CREATE_RESPONSE` → the **#629 mapping** (already correct): OK → re-list +
   select the new character + return to roster; a typed rejection → surface the
   honest message on the creation view (name taken / invalid race / invalid class
   / invalid name / server error / max characters) and stay on the creation view
   so the user can fix and retry.
6. **Cancel** → `creation_cancelled` → controller returns to the roster, no net
   traffic.

## Preserved invariants (must not regress)

- Name length cap (32), race/class validation against the frozen roster.
- Appearance record is opaque-but-bounded (hair/face/skin), never gameplay-
  authoritative; the existing `MeridianAppearance` default + normalise path.
- The #629 `CharCreateStatus` → message mapping (mirrors `world.fbs` 1:1) — moved/
  reused, not rewritten. Its regression lock in `char_select_verify.gd` stays green
  (extend it for the new view wiring, don't weaken it).
- Empty-roster "create one to enter the world" prompt.
- CharList INTERNAL (#479) load-failure handling stays.
- #630 window scaling: both views scale with the window (they live under the same
  `canvas_items` stretch; use responsive anchors/containers, no fixed pixel traps).

## Testing

- **`char_select_verify.gd`** (existing GDScript verify) extended to cover:
  - Roster view shows list + Delete / Create New Character / Enter Realm; the
    "Enter Realm" label is present; selection gating unchanged.
  - Create New Character reveals the creation view and hides the roster; Cancel
    returns to the roster with no net traffic.
  - `character_confirmed` carries the chosen name/race/class/appearance; the
    controller's create path fires with those values.
  - The #629 status→message mapping still asserts exact text (keep the existing
    regression lock; re-point it at wherever the message now renders).
- **`CharacterCreate` unit-tested in isolation** (no session): pickers populate,
  the two signals fire with correct payloads, name validation surfaces locally.
- **Runtime E2E** in the Godot client against a local realm: create a 2nd
  character through the new flow; delete; enter realm; verify drag-to-rotate and
  live paperdoll updates; verify both views scale on window resize.

## Out of scope (YAGNI / future)

- Arrow-cycler selectors, appearance thumbnails, richer customization (dropdowns
  stay).
- Character reordering, per-slot cosmetics, "boost"/paid slots.
- Any server/protocol change — this is client UI only.
- Login screen sizing — that's the sibling story #638.

## Provenance

Clean-room UI work; reuses existing Meridian client code (paperdoll builder,
appearance pickers, net frame builders). No third-party assets or code introduced.
