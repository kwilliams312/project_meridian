#!/usr/bin/env python3
"""`python -m meshy` — Meshy.ai intake CLI (spec ④ §7, TD-09 provenance).

Subcommands:

  generate   Generate a `.glb` via Meshy (text or image prompt), download it,
             and land it under `content/<ns>/assets/art/<name>/` with an IF-8
             sidecar (`source_tier: ai`, `restyle_status: pending`) and an
             auditable prompts file — TD-09's provenance requirements applied
             automatically instead of by hand.

  convert-rig  Convert a Meshy auto-rigged humanoid `.glb` onto the canonical
             Meridian skeleton (spec ④ §7.3): map Meshy bones → canonical bones
             via the versioned `bone_map.yaml`, merge helper/twist bones into
             their nearest mapped ancestor, re-bind to the generator's canonical
             armature, and re-export. Runs a headless-Blender pass; the map/plan
             refusal gates run first in pure Python (no Blender needed to reject
             a bad request).

`generate`'s refusal gates (both exit 2, before any network call):
  * `MESHY_API_KEY` is unset.
  * `--terms-verified` was not passed — Meshy's commercial-terms check must be
    operator-confirmed at time of use (TD-09 precondition, spec §7.1).

`convert-rig`'s refusal / error gates (before any Blender spawn):
  * unknown `--meshy-version` → exit 2 naming the known versions.
  * missing input `.glb` → exit 2.
  * unmapped Meshy bones → exit 1 listing them (the map grows deliberately).
  * an UNVERIFIED map version without `--allow-unverified-map` → exit 2.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

import yaml

from . import client as client_mod
from . import intake
from . import mapping

REPO_ROOT = Path(__file__).resolve().parent.parent.parent


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="meshy", description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    gen = sub.add_parser(
        "generate",
        help="generate + fetch an AI asset via Meshy",
        epilog=(
            "Budget pre-check: only lod0_tris (triangle count) is checked "
            "against the class ceiling before landing; texture_max_px and "
            "vram_mb are NOT inspected at intake — declare them by hand in "
            "the sidecar if known (the schema/lint gate still enforces them)."
        ),
    )
    prompt_group = gen.add_mutually_exclusive_group(required=True)
    prompt_group.add_argument("--text", help="text prompt for text-to-3D generation")
    prompt_group.add_argument(
        "--image",
        help=(
            "image for image-to-3D generation: an http(s) URL, or a local "
            ".png/.jpg/.jpeg file (base64-encoded into a data URI)"
        ),
    )
    gen.add_argument("--ns", required=True, help="pack namespace, e.g. 'core'")
    gen.add_argument("--name", required=True, help="asset name, lowercase snake_case")
    gen.add_argument(
        "--class",
        dest="asset_class",
        required=True,
        help="IF-8 sidecar class, e.g. 'character_model'",
    )
    gen.add_argument(
        "--terms-verified",
        action="store_true",
        help="confirm Meshy's commercial-terms check was verified for this use (TD-09)",
    )
    gen.add_argument(
        "--authors",
        default="meridian-contributors",
        help="comma-separated authors recorded in provenance.authors",
    )
    gen.add_argument(
        "--content-root",
        type=Path,
        default=REPO_ROOT / "content",
        help="content tree root (default: repo content/)",
    )
    gen.add_argument(
        "--target-polycount",
        type=int,
        default=None,
        help=(
            "override the Meshy target_polycount request (text-to-3D only). "
            "Default: derived from the --class lod0_tris ceiling in "
            "budgets.json with headroom. May not exceed the class ceiling."
        ),
    )
    gen.add_argument(
        "--poll-interval", type=float, default=client_mod.DEFAULT_POLL_INTERVAL_S
    )
    gen.add_argument(
        "--poll-timeout", type=float, default=client_mod.DEFAULT_POLL_TIMEOUT_S
    )

    conv = sub.add_parser(
        "convert-rig",
        help="convert a Meshy auto-rig onto the canonical skeleton (spec ④ §7.3)",
        epilog=(
            "The map/plan gates (unknown version, missing glb, unmapped bones, "
            "unverified map) run in pure Python before any Blender spawn — CI "
            "never runs Blender and never needs a live Meshy account."
        ),
    )
    conv.add_argument("input", help="the Meshy-rigged source .glb to convert")
    conv.add_argument(
        "--meshy-version",
        required=True,
        help="Meshy model version keying bone_map.yaml (e.g. 'meshy-5')",
    )
    conv.add_argument("--out", required=True, help="output canonical-rig .glb path")
    conv.add_argument(
        "--allow-unverified-map",
        action="store_true",
        help=(
            "DEVELOPMENT ONLY: convert even though the bone map version is "
            "verified: false. Without it, an unverified map is refused so a "
            "guessed table cannot silently mis-convert."
        ),
    )
    conv.add_argument(
        "--blender",
        default=None,
        help="Blender binary (default: $BLENDER or 'blender' on PATH)",
    )
    conv.add_argument(
        "--blender-timeout",
        type=float,
        default=300.0,
        help="hard timeout (seconds) for the headless Blender pass",
    )
    return parser


def _origin_url(task_id: str, endpoint: str) -> str:
    """The Meshy task's canonical API URL — the auditable origin for provenance."""
    return f"{client_mod.BASE_URL}{endpoint}/{task_id}"


def _new_client(api_key: str) -> client_mod.MeshyClient:
    """Client construction seam — tests monkeypatch this to inject a mock transport."""
    return client_mod.MeshyClient(api_key)


def cmd_generate(args: argparse.Namespace) -> int:
    # --- Refusal gates: BEFORE any network call (TD-09 precondition). ---
    if not args.terms_verified:
        print(
            "refused: --terms-verified is required — Meshy's commercial-terms "
            "check must be operator-confirmed before generating (TD-09)",
            file=sys.stderr,
        )
        return 2

    api_key = os.environ.get("MESHY_API_KEY")
    if not api_key:
        print("refused: MESHY_API_KEY is not set", file=sys.stderr)
        return 2

    try:
        intake.validate_namespace(args.ns)
        intake.validate_name(args.name)
        image_url = intake.image_ref_to_url(args.image) if args.image else None
    except intake.IntakeError as exc:
        print(f"refused: {exc}", file=sys.stderr)
        return 2

    budgets = intake.load_budgets()
    paths = intake.landing_paths(
        content_root=args.content_root,
        ns=args.ns,
        name=args.name,
        asset_class=args.asset_class,
        budgets=budgets,
    )

    # --- Derive the Meshy target_polycount from the class budget (issue #627),
    # still before any network call. Only text-to-3D takes a polycount, so an
    # override paired with --image is refused rather than silently dropped.
    target_polycount: int | None = None
    if args.text:
        try:
            target_polycount = intake.derive_target_polycount(
                args.asset_class, budgets, override=args.target_polycount
            )
        except intake.IntakeError as exc:
            print(f"refused: {exc}", file=sys.stderr)
            return 2
    elif args.target_polycount is not None:
        print(
            "refused: --target-polycount applies to --text generation only "
            "(image-to-3D does not take a polycount)",
            file=sys.stderr,
        )
        return 2

    client = _new_client(api_key)
    try:
        try:
            if args.text:
                handle = client.text_to_3d(
                    args.text,
                    target_polycount=target_polycount,
                    poll_interval_s=args.poll_interval,
                    poll_timeout_s=args.poll_timeout,
                )
                endpoint = client_mod.TEXT_TO_3D_PATH
            else:
                handle = client.image_to_3d(image_url)
                endpoint = client_mod.IMAGE_TO_3D_PATH
        except (client_mod.MeshyAPIError, client_mod.MeshyPollTimeoutError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1

        try:
            status = client.poll(
                handle.task_id,
                endpoint=endpoint,
                interval_s=args.poll_interval,
                timeout_s=args.poll_timeout,
            )
        except (client_mod.MeshyAPIError, client_mod.MeshyPollTimeoutError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1

        if not status.succeeded:
            print(
                f"error: Meshy task {handle.task_id} ended in status "
                f"{status.status} (expected SUCCEEDED)",
                file=sys.stderr,
            )
            return 1

        try:
            client.download(
                handle.task_id, paths.glb_path, status=status, endpoint=endpoint
            )
        except client_mod.MeshyAPIError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1

        # --- Budget pre-check: fail before any sidecar is written. ---
        lod0_tris = intake.count_glb_triangles(paths.glb_path)
        precheck_doc = intake.build_budget_precheck_doc(args.asset_class, lod0_tris)
        budget_errors = _check_budget(precheck_doc, paths.sidecar_path)
        if budget_errors:
            paths.glb_path.unlink(missing_ok=True)
            print("refused: budget pre-check failed —", file=sys.stderr)
            for err in budget_errors:
                print(f"  {err}", file=sys.stderr)
            return 1

        # --- Land the prompts file + sidecar (only now that budget passed). ---
        prompts_doc = intake.build_prompts_doc(
            task_id=handle.task_id,
            preview_task_id=handle.preview_task_id,
            request_payload=handle.request_payload,
            model_version=client.model_version,
        )
        sidecar_doc = intake.build_sidecar(
            asset_id=intake.asset_id(args.ns, args.name),
            asset_class=args.asset_class,
            source=paths.source,
            model_version=client.model_version,
            prompts_file=paths.prompts_path.name,
            origin_url=_origin_url(handle.task_id, endpoint),
            authors=[a.strip() for a in args.authors.split(",") if a.strip()],
            lod0_tris=lod0_tris,
        )

        paths.prompts_path.write_text(
            yaml.safe_dump(prompts_doc, sort_keys=False), encoding="utf-8"
        )
        paths.sidecar_path.write_text(
            yaml.safe_dump(sidecar_doc, sort_keys=False), encoding="utf-8"
        )
    finally:
        client.close()

    print(f"OK — landed {intake.asset_id(args.ns, args.name)} at {paths.asset_dir}")
    return 0


def cmd_convert_rig(args: argparse.Namespace) -> int:
    """Convert a Meshy auto-rig onto the canonical skeleton (spec ④ §7.3).

    All refusal / error gates run here in pure Python — the Blender pass is only
    reached once the request is proven valid, so CI (no Blender, no live Meshy)
    can exercise every rejection path.
    """
    # --- Version/map gate: BEFORE touching the glb. MappingError also covers a
    # structurally invalid map (e.g. non-boolean `verified:`) — refuse rather
    # than let a malformed flag unlock the unverified-map gate.
    try:
        bone_map = mapping.load_map(args.meshy_version)
    except mapping.MappingError as exc:
        print(f"refused: {exc}", file=sys.stderr)
        return 2

    in_glb = Path(args.input)
    if not in_glb.is_file():
        print(f"refused: input glb {in_glb} does not exist", file=sys.stderr)
        return 2

    try:
        meshy_bones = mapping.joint_names_from_glb(in_glb)
    except Exception as exc:  # noqa: BLE001 — any parse failure is a bad input
        print(
            f"error: could not read rig joints from {in_glb} ({exc})",
            file=sys.stderr,
        )
        return 1

    conv_plan = mapping.plan_with_map(meshy_bones, bone_map)

    # --- Unmapped bones are a hard, listed error (the map grows deliberately). ---
    if conv_plan.unmapped:
        print(
            "error: unmapped Meshy bones for version "
            f"{args.meshy_version!r} — add them to tools/meshy/bone_map.yaml "
            "(rename target) or let them merge under a mapped ancestor: "
            + ", ".join(sorted(conv_plan.unmapped)),
            file=sys.stderr,
        )
        return 1

    # --- Unverified-map gate: a guessed table must not silently mis-convert. ---
    if not bone_map.verified and not args.allow_unverified_map:
        print(
            f"refused: bone map version {args.meshy_version!r} is UNVERIFIED "
            "(verified: false) — its Meshy bone names have not been reconciled "
            "against a real sample. Verify the map, or pass "
            "--allow-unverified-map to convert anyway (development only).",
            file=sys.stderr,
        )
        return 2

    # --- Audit trail: echo every resolved merge pair BEFORE the Blender pass —
    # a weight merge must never happen silently (PR #523 review).
    for helper, target in sorted(conv_plan.merges.items()):
        print(f"merge: {helper} -> {target}")

    return _run_blender_conversion(
        in_glb,
        Path(args.out),
        args.meshy_version,
        conv_plan,
        blender=args.blender,
        timeout_s=args.blender_timeout,
    )


def _run_blender_conversion(  # pragma: no cover - spawns Blender (never in CI)
    in_glb: Path,
    out_glb: Path,
    version: str,
    conv_plan: mapping.ConversionPlan,
    *,
    blender: str | None = None,
    timeout_s: float = 300.0,
) -> int:
    """Run the headless-Blender conversion pass — the only non-pure step.

    Seam: tests monkeypatch this to assert the gate logic without Blender. The
    already-resolved plan crosses the subprocess boundary as JSON (Blender's
    bundled Python has no PyYAML, so the map is never loaded inside Blender), and
    output-file existence is the authoritative success signal — Blender exits 0
    even when a ``--python`` script raises, so the return code alone is not
    trusted.
    """
    import json
    import tempfile

    _ = version  # recorded by the caller's log line; not needed inside Blender
    blender_bin = blender or os.environ.get("BLENDER", "blender")
    script = Path(__file__).resolve().parent / "convert_rig.py"

    out_glb.parent.mkdir(parents=True, exist_ok=True)
    out_glb.unlink(missing_ok=True)  # so a stale file can't fake success

    with tempfile.NamedTemporaryFile(
        "w", suffix=".plan.json", delete=False, encoding="utf-8"
    ) as fh:
        json.dump({"renames": conv_plan.renames, "merges": conv_plan.merges}, fh)
        plan_json = fh.name

    cmd = [
        blender_bin,
        "--background",
        "--factory-startup",
        "-noaudio",
        "--python",
        str(script),
        "--",
        "--in",
        str(in_glb),
        "--out",
        str(out_glb),
        "--plan-json",
        plan_json,
    ]

    try:
        proc = subprocess.run(cmd, timeout=timeout_s, capture_output=True, text=True)
    except FileNotFoundError:
        print(
            f"error: Blender binary {blender_bin!r} not found — set --blender or "
            "$BLENDER (convert-rig needs a headless Blender; spec ④ §7.3)",
            file=sys.stderr,
        )
        return 1
    except subprocess.TimeoutExpired:
        print(
            f"error: Blender conversion timed out after {timeout_s}s", file=sys.stderr
        )
        return 1
    finally:
        Path(plan_json).unlink(missing_ok=True)

    if proc.returncode != 0 or not out_glb.is_file():
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        print(
            f"error: Blender conversion did not produce {out_glb} "
            f"(exit {proc.returncode})",
            file=sys.stderr,
        )
        return 1

    print(
        f"OK — converted {in_glb} → {out_glb} ({len(conv_plan.renames)} renamed, "
        f"{len(conv_plan.merges)} merged)"
    )
    return 0


def _check_budget(doc: dict, rel_path: Path) -> list[str]:
    """Delegates to validate_content.check_budget (repo's single budget-lint source).

    `validate_content` is a top-level sibling module under `tools/`, not part of
    the `meshy` package — imported lazily here (rather than at module load) so
    a bare `import meshy.client` (no CLI use) never requires `tools/` on
    `sys.path`. By the time `generate` runs, `meshy` itself was only importable
    because some `sys.path` entry resolves to `tools/`, so this import already
    finds `validate_content` there without further path manipulation.
    """
    import validate_content

    return validate_content.check_budget(doc, rel_path)


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    if args.command == "generate":
        return cmd_generate(args)
    if args.command == "convert-rig":
        return cmd_convert_rig(args)
    parser.error(f"unknown command: {args.command}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
