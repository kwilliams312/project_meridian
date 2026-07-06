# `mcc fmt` — canonical content-YAML form

`mcc fmt` rewrites hand-authored `/content` YAML into one **canonical** form so
Git diffs are semantic, not cosmetic, and CI / pre-commit can enforce a single
house style (`mcc fmt --check`). It is the content analogue of `gofmt` / `black`.

**Contract:** Tools PRD §2.1 ("canonical key order enforced by a formatter"),
§7.2 (`mcc fmt --check` as the first pre-merge gate) · Tools SAD §5.3/§6.2
("the same `mcc fmt` emitter"; "canonical form narrows the input space").

## Guarantees

Both are property-tested (`tools/mcc/tests/test_format.cpp`) and re-checked
against every real file under `content/core/`:

1. **Idempotent** — `fmt(fmt(x)) == fmt(x)`, byte-for-byte.
2. **Semantics-preserving** — `parse(fmt(x))` is node-identical to `parse(x)`:
   same keys, same values, and same *resolved scalar types*. A string that
   looks like a number/bool/null (e.g. `"4.6"`, `"01"`, `"on"`) keeps its
   string type; a bare `4.6` / `true` / `null` keeps its non-string type.

`mcc fmt` never changes what a file *means*. It only changes how it is spelled.

## Canonical-form rules

### Key order
- **Envelope first, fixed order:** `schema`, then `id`, then `namespace` (pack
  manifests). These identify the document and always lead — Tools SAD §2.1
  "envelope-first."
- **Remaining keys keep their source order.** A formatter that only fixes the
  envelope and the *shape* is the safe, non-surprising choice: it never reorders
  keys an author grouped meaningfully. (A full schema-property sort — reading
  the `properties:` order from `schema/content/*.schema.yaml` — is a documented
  future extension; it requires loading the schemas and is deferred so the
  formatter stays a pure, schema-independent pass for now.)

### Indentation & whitespace
- **Two spaces per level**, block style. Sequence `- ` markers are indented one
  step under their owning key.
- **No trailing whitespace** on any line.
- **Exactly one trailing newline**; no blank lines inside a document.

### Collection style (flow vs block)
- A map/sequence is emitted in **flow style** (`{ min: 3, max: 4 }`,
  `[a, b, c]`) **iff it was authored in flow AND all its children are scalars**
  (a "leaf" collection). This preserves the established house style for compact
  fixed-shape data (`intRange`, `position`, small string lists) while producing
  canonical spacing: `{ k: v, k: v }` and `[a, b, c]`.
- Any collection with a **non-scalar child** is emitted in **block** style —
  structure is never squeezed onto one line.
- Empty collections render as `{}` / `[]`.

### Scalars & quoting
- **Explicitly-quoted scalars stay quoted.** yaml-cpp marks a source scalar that
  was quoted or a block scalar with tag `!`; `mcc fmt` re-emits it double-quoted.
  This preserves author intent (`"0.1.0"`, `"4/4"`, `"4.6"`) and guarantees a
  string never silently resolves to a number/bool on re-parse.
- **Plain scalars stay plain** unless a leading indicator, an embedded `": "`, a
  leading/trailing space, or a control character would make the plain form
  ambiguous — then they are double-quoted with minimal escaping.
- **Null** normalizes to the word `null` (from `~`, `Null`, empty, etc.).

### Comments
- **Leading full-line comments** (a run of `#` lines directly above a key) and
  **trailing end-of-line comments** (`key: value  # note`) are preserved and
  re-attached to the key they annotate. Trailing comments are normalized to two
  spaces before the `#`.
- Blank-line separators between a comment block and later content are dropped
  (canonical form has no blank lines); the comment stays with the content that
  follows it.

### Banned constructs
- **Anchors / aliases** (`&a` / `*a`) are not part of the canonical form
  (Tools SAD §6.2 "anchors are banned by `mcc fmt`"). Content files do not use
  them; a file relying on them is outside the canonical input space.

## Known v1 limitation — block scalars

Folded (`>-`) and literal (`|`) block scalars are re-emitted as **double-quoted
single-line strings**. The value is identical (semantics preserved, idempotent),
but the multi-line authoring layout is lost — long prose fields (`offer_text`,
`gossip_text`) become one long quoted line.

**Why:** yaml-cpp's parsed node model does not distinguish a folded/literal/
quoted scalar (all carry tag `!`), so `mcc fmt` cannot faithfully reconstruct a
block scalar from the node tree alone. Preserving block-scalar layout requires
the concrete-syntax-tree (CST) layer the Tools SAD (§6.2) assigns to the Codex
editor; wiring that into `mcc fmt` is tracked as a follow-up. Until then the
formatter chooses the **semantically safe** representation over the pretty one.

## CLI

```sh
mcc fmt [path]            # format in place (default path: ./content)
mcc fmt --check [path]    # report drift, exit 1 if any file is non-canonical;
                          # writes nothing (CI / pre-commit gate)
```

`path` may be a single `.yaml` file or a directory (recursively formats every
`*.yaml`, in sorted order for deterministic output). Exit codes: `0` clean /
formatted, `1` drift found under `--check`, `2` I/O or parse error.
