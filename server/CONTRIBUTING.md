# Contributing to `/server` — Clean-Room Enforcement Checklist

> **Status: DRAFT — pending owner approval (#156).**
> This is a proposed server-specific enforcement layer for review. Nothing here is
> binding until the project owner approves it. The split between *written* provenance
> statements and *attestation* checkboxes (§4) is a recommended default the owner can
> tighten or loosen — call out anything you'd change on the PR.

This document **extends** the root [CONTRIBUTING.md](../CONTRIBUTING.md); it does not
replace it. The root file states the binding clean-room *policy* (no GPL code enters the
Apache-2.0 tree; contributors with recent CMaNGOS/TrinityCore exposure implement from the
written specs in `/docs`, not from source recall). This file adds the server track's
*enforcement mechanics*: how that policy is proven, cited, and checked on every server PR.

Read the root policy first. Everything below assumes it.

**Why the server track needs its own checklist.** Clean-room discipline is the server
track's **#1 legal risk** — the single highest-severity item in the [Server PRD §10 Risks](../docs/prd/server-prd.md#risks)
list. The server is *architecturally inspired by* CMaNGOS and TrinityCore (both GPL):
authd/worldd split, three-DB layout, grid/cell spatial partitioning, a fixed-cadence
update loop, opcode-style dispatch ([Server PRD §1](../docs/prd/server-prd.md), [§2.7](../docs/prd/server-prd.md)).
That is exactly the code most at risk of accidental structural derivation. The PRD names
the mitigation explicitly (§10 Risk 1): *written specs before implementation, contribution
policy in `/server/CONTRIBUTING.md`, review checklist, provenance statements on
architecture-heavy PRs.* This file is that mitigation, made concrete.

---

## 1. Written-specs-before-implementation rule

**Every server subsystem traces to a written spec in `/docs` before any code is written.**
We study CMaNGOS/TrinityCore public architecture as prior art, but the artifact you
implement from is the Meridian spec — never source recall, never a decompilation, never a
"how does TrinityCore do this" read of their tree.

The spec chain, in priority order:

| Layer | Document | What it fixes |
|-------|----------|---------------|
| Baseline | [`docs/00-GAME-DESIGN-BASELINE.md`](../docs/00-GAME-DESIGN-BASELINE.md) | Feature IDs, milestones, technical decisions (TD-01..TD-12) |
| Cross-track decisions | [`docs/01-SYNC-DECISIONS.md`](../docs/01-SYNC-DECISIONS.md) | Binding interpretations (D-01..D-32) and open items |
| PRD (*what*) | [`docs/prd/server-prd.md`](../docs/prd/server-prd.md) | Server scope, behavior, milestones |
| SAD (*how*) | [`docs/sad/server-sad.md`](../docs/sad/server-sad.md) | Component boundaries, threads, data flow, the server side of each interface (IF-1..IF-10) |
| Interface contracts | [`/schema`](../schema) | The wire/DB shapes — protocol `.fbs`, DB schemas |

**Rule:** if the subsystem you are building is not described by a written spec in `/docs`,
the spec comes first. Do not implement ahead of the spec and backfill the doc — that
inverts the clean-room guarantee (you'd be coding from recall, then rationalizing). If a
spec is missing or ambiguous, open a docs PR or a Sync-Decisions item **before** the
implementation PR, so the design is on record as an original Meridian design.

### How to cite the spec in the PR

Every server PR description includes a **Spec citation** line pointing at the written
source it implements:

```
Spec: Server SAD §5.1 (IF-1 auth flow) + docs/01-SYNC-DECISIONS.md D-01 (FlatBuffers)
```

- Cite by **document + section/decision ID** (e.g. `Server SAD §4.7`, `Server PRD §2.4`,
  `D-23`, `IF-2`), not just "the SAD."
- If the PR implements a schema/interface, cite the `/schema` file too (e.g.
  `/schema/net/auth.fbs`).
- If no spec existed and you added one, cite the docs PR that introduced it.

The recent auth-path PRs are the reference pattern here — e.g. *"SRP6a auth service —
clean-room from RFC 5054"* traced its algorithm to a public IETF RFC, not to any GPL
implementation. Cite the public standard or the Meridian spec; never a GPL codebase.

---

## 2. PR-review checklist (reviewers apply this to every server PR)

Reviewers paste this into the review and check each box before approving. It is a
**gate**, not advisory — an unchecked box blocks merge.

```
Clean-room review — server PR

[ ] Spec citation present. The PR cites a written /docs spec (SAD §, PRD §, or a
    decision ID) — or a public standard/RFC — for what it implements.
[ ] No GPL source referenced. Nothing in the PR, its description, or its commit
    messages cites, links, quotes, or paraphrases CMaNGOS / TrinityCore / any other
    GPL source. No "matches how TrinityCore does it" reasoning.
[ ] Original design confirmed. Protocol constants, opcode values, table layouts,
    struct field orders, and magic numbers are Meridian's own (schema-generated where
    applicable, TD-07) — not transcribed from a GPL project.
[ ] Interfaces match the SAD. Any interface touched (IF-1..IF-10) matches the
    contract in the Server SAD §5 / §1.1 and the /schema definition. Deviations are
    called out and justified, or the SAD is updated in the same or a linked PR.
[ ] Provenance statement present where required (see §3–§4): a written provenance
    statement on architecture-heavy PRs, or the attestation checkbox on routine PRs.
[ ] Exposure disclosed. If the author has recent CMaNGOS/TrinityCore exposure and this
    is architecture-heavy, that is disclosed (§5) and the provenance statement addresses it.
[ ] Suspicion → reject. If anything reads as derived (uncharacteristic structure,
    verbatim constants, comments echoing another project), the PR is held for
    discussion, not merged. (Root policy: "Any contribution suspected of derivation is
    rejected in review.")
```

A reviewer who cannot check "No GPL source referenced" and "Original design confirmed"
with confidence should **request the author's provenance statement** (§3) before
proceeding, even on a PR that would otherwise count as routine.

---

## 3. Provenance statements

A **provenance statement** is a short written attestation of where a change's design came
from. It is the audit trail that lets us defend the clean-room claim after the fact.

### 3.1 Architecture-heavy PRs — written statement REQUIRED

A PR is **architecture-heavy** if it shapes or implements any of:

- the **data model** — DB schema, table layout, character/world/auth record shapes
  (Server SAD §4);
- the **protocol** — wire format, message framing, opcode/handler tables, session/handshake
  flow (IF-1/IF-2/IF-3, `/schema/net/*`);
- the **core loop** — the fixed-cadence update loop, tick orchestration, threading model,
  grid/cell AoI, the message bus, save-ownership fencing (Server SAD §2, §3, §4.7).

These are precisely the areas where CMaNGOS/TrinityCore structural influence is most
likely and most dangerous. They require a **written provenance statement** in the PR
description, in the shape the root policy already models:

> **Provenance:** Designed from Server SAD §2.4 (grid/cell AoI). Grid sizing is
> Meridian's own (533 m grids, 8×8 cells). Informed by public CMaNGOS *documentation*
> of the grid-activation concept; **no CMaNGOS/TrinityCore source was consulted or
> copied.** Opcode/handler structure is schema-generated from `/schema` (TD-07).

A good statement says: (a) which spec it was built from, (b) what is original to Meridian,
(c) what public prior-art *concepts* informed it (if any), and (d) an explicit
"no GPL source consulted or copied" line.

### 3.2 Routine PRs — checkbox attestation

Routine PRs (bug fixes, tests, config, tooling, docs, refactors that don't reshape the
data model / protocol / core loop) do not need a paragraph. They carry a one-line
attestation checkbox in the PR template:

```
[ ] Clean-room: implemented from Meridian specs; no GPL source consulted or copied.
```

### 3.3 Proposed default (owner to ratify)

**This written-vs-checkbox split is a recommendation, not a settled rule.** The proposed
default is: *written statement for data-model / protocol / core-loop PRs; checkbox for
everything else.* The owner may tighten it (written statement on **all** server PRs) or
loosen it (checkbox suffices unless a reviewer requests more). Whatever is chosen, the
PR template should encode it so contributors can't skip the step. Flag your preference
on the approval PR (#156).

---

## 4. PR template additions (proposed)

To make §1–§3 self-enforcing, add these fields to the server PR template
(`.github/PULL_REQUEST_TEMPLATE`) once approved:

```markdown
### Clean-room (server)
- **Spec citation:** <doc + section/decision ID, or public standard>
- **Provenance:** <written statement for architecture-heavy PRs — data model / protocol /
  core loop — else check the box below>
- [ ] Clean-room: implemented from Meridian specs; no GPL source consulted or copied.
```

Editing that template is out of scope for this draft (it would touch an existing file and
risk conflicts with parallel work). It is listed here as the follow-up the owner approves
alongside this checklist.

---

## 5. Guidance for contributors with recent CMaNGOS/TrinityCore exposure

Prior CMaNGOS/TrinityCore experience is **valuable** — it is why the architecture is
sound. It is also the highest-risk contributor profile for accidental derivation. You can
contribute safely; here is how.

**Do:**

- **Work forward from the spec.** Open the relevant `/docs` spec and implement *that*.
  If you find yourself reconstructing "how the other project laid this out," stop — that
  is source recall, and it is exactly what the clean-room policy forbids.
- **Disclose your exposure** on architecture-heavy PRs. A one-line note in the PR
  ("I have recent TrinityCore experience; this was built from Server SAD §X, no source
  consulted") is not an admission of wrongdoing — it is the disclosure that lets the
  reviewer apply the right scrutiny and that protects the project's clean-room record.
- **Reach for public prior art in the abstract.** Public architecture *documentation*,
  RFCs, and papers are fine to cite. The concept ("grid activation," "SRP6a") is not
  copyrightable; a specific implementation is.
- **Prefer generated code over hand-transcription.** Opcode tables and wire structs are
  schema-generated from `/schema` (TD-07) precisely so no one hand-copies constants.

**Avoid:**

- **Structure-copying.** Do not reproduce another project's file organization, class
  decomposition, function breakdown, or table column order from memory. Two clean-room
  implementations of the same spec will differ in structure; suspicious *sameness* is the
  red flag reviewers look for.
- **Verbatim constants and magic numbers** lifted from a GPL tree (packet opcodes, table
  names, timing constants, protocol version bytes). Meridian's are its own.
- **"Just checking how they did it."** Do not open the CMaNGOS/TrinityCore source while
  implementing a Meridian subsystem — not even to compare. If you're stuck on the spec,
  raise a docs/Sync-Decisions question instead.
- **Porting.** Never translate a GPL function into our style and call it new. A port is a
  derivative work; it does not become Apache-2.0 by being retyped.

If you are unsure whether something crosses the line, ask **before** you write it. A
question is cheap; a tainted subsystem that has to be ripped out and re-implemented is not.

---

## 6. In force before further substantive server PRs

The server track is already underway — the M0 auth-path PRs (TLS listener #77, SRP6a
service #78, login flow #79, account CLI #80, and the DB-layer/schema PRs) **have already
merged**. This checklist is therefore **catching up**: it needs to be in force *before the
next substantive server PR* (the world loop, protocol, data-model, and core-loop work
ahead), so the highest-risk architecture lands under the enforcement it was always meant
to have.

The already-merged auth-path work was itself built clean-room from public standards
(e.g. SRP6a from RFC 5054, not from a GPL implementation), consistent with this policy; no
retroactive re-review is proposed here unless the owner wants one. The point of this
document is to make the discipline explicit and enforceable **going forward**, at the
moment the server track moves from auth into its architecture-heavy core.

---

*This is a DRAFT for owner approval (#156). Once approved, drop the status banner, wire the
PR-template fields (§4), and this becomes the binding server-track enforcement layer under
the root clean-room policy.*
