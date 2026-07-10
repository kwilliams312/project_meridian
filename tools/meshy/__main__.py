#!/usr/bin/env python3
"""`python -m meshy` — Meshy.ai intake CLI (spec ④ §7, TD-09 provenance).

Subcommands:

  generate   Generate a `.glb` via Meshy (text or image prompt), download it,
             and land it under `content/<ns>/assets/art/<name>/` with an IF-8
             sidecar (`source_tier: ai`, `restyle_status: pending`) and an
             auditable prompts file — TD-09's provenance requirements applied
             automatically instead of by hand.

`convert-rig` is a separate story (#506, task ④/T7) — not implemented here;
this module is structured so that subcommand can be added to `_build_parser`
without touching `generate`.

Refusal gates (both exit 2, before any network call):
  * `MESHY_API_KEY` is unset.
  * `--terms-verified` was not passed — Meshy's commercial-terms check must be
    operator-confirmed at time of use (TD-09 precondition, spec §7.1).
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

import yaml

from . import client as client_mod
from . import intake

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
        "--poll-interval", type=float, default=client_mod.DEFAULT_POLL_INTERVAL_S
    )
    gen.add_argument(
        "--poll-timeout", type=float, default=client_mod.DEFAULT_POLL_TIMEOUT_S
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

    client = _new_client(api_key)
    try:
        try:
            if args.text:
                handle = client.text_to_3d(
                    args.text,
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
    parser.error(f"unknown command: {args.command}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
