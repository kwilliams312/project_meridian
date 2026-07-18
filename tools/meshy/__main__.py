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

  rig        Auto-rig a humanoid `.glb` via Meshy (`POST /openapi/v1/rigging`)
             and download the rigged glb (optionally its bundled walk/run
             basic_animations). Same TD-09 `--terms-verified` + pinned-version
             gate as `generate`.

  animate    Retarget a documented library `--action-id` onto a completed
             rigging task (`POST /openapi/v1/animations`) and download the
             animation glb. Same TD-09 gate.

  Output of `rig`/`animate` is RAW AI-generated content (TD-09 quarantine:
  `source_tier: ai`, `restyle_status: pending`) — write `--out` OUTSIDE the
  mergeable content tree; never stage it as pack content.

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
import contextlib
import hashlib
import io
import json
import os
import secrets
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import yaml

from . import client as client_mod
from . import intake
from . import locking
from . import mapping
from . import protocol

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
_JSON_SINK_FAILURE_MESSAGE = (
    "error: Meshy JSON event sink failed; refusing further stdout output"
)


def _report_json_sink_failure() -> int:
    """Report a poisoned stdout sink without reflecting dynamic data."""

    print(_JSON_SINK_FAILURE_MESSAGE, file=sys.stderr)
    return 1


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
    gen.add_argument(
        "--model-version",
        default=client_mod.DEFAULT_MODEL_VERSION,
        help=(
            "explicit Meshy model release recorded in provenance "
            f"(default: {client_mod.DEFAULT_MODEL_VERSION}; 'latest' is refused)"
        ),
    )
    gen.add_argument(
        "--timeout",
        type=float,
        default=None,
        help=(
            "per-request HTTP timeout in seconds for each Meshy create/poll/"
            "download call — NOT the overall poll budget (see --poll-timeout). "
            "Overrides $MESHY_TIMEOUT; default keeps a short connect but a "
            f"generous {client_mod.DEFAULT_HTTP_TIMEOUT_S:.0f}s read (issue #735)."
        ),
    )
    gen.add_argument(
        "--json-events",
        action="store_true",
        help="emit only versioned JSON-lines job events on stdout",
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

    _add_rig_parser(sub)
    _add_animate_parser(sub)
    return parser


def _add_common_meshy_call_args(p: argparse.ArgumentParser) -> None:
    """Args shared by every LIVE Meshy-calling subcommand (rig/animate).

    The same TD-09 gate (`--terms-verified`) + pinned-version rule the `generate`
    command enforces, plus the poll/timeout knobs. Output of these commands is
    RAW Meshy AI output → TD-09 quarantine (`source_tier: ai`,
    `restyle_status: pending`): it must NOT be staged as mergeable pack content.
    """
    p.add_argument(
        "--out",
        type=Path,
        required=True,
        help=(
            "destination path for the downloaded result .glb. This is RAW "
            "AI-generated output (TD-09 quarantine) — write it OUTSIDE the "
            "mergeable content tree, not into content/<ns>/assets/."
        ),
    )
    p.add_argument(
        "--terms-verified",
        action="store_true",
        help="confirm Meshy's commercial-terms check was verified for this use (TD-09)",
    )
    p.add_argument(
        "--model-version",
        default=client_mod.DEFAULT_MODEL_VERSION,
        help=(
            "explicit pinned Meshy model release (default: "
            f"{client_mod.DEFAULT_MODEL_VERSION}; 'latest' is refused)"
        ),
    )
    p.add_argument(
        "--poll-interval", type=float, default=client_mod.DEFAULT_POLL_INTERVAL_S
    )
    p.add_argument(
        "--poll-timeout", type=float, default=client_mod.DEFAULT_POLL_TIMEOUT_S
    )
    p.add_argument(
        "--timeout",
        type=float,
        default=None,
        help=(
            "per-request HTTP timeout in seconds (NOT the overall poll budget; "
            "see --poll-timeout). Overrides $MESHY_TIMEOUT."
        ),
    )


def _add_rig_parser(sub) -> None:
    rig = sub.add_parser(
        "rig",
        help="auto-rig a humanoid mesh via Meshy (POST /openapi/v1/rigging)",
        epilog=(
            "Feed a GENERIC humanoid mesh — auto-rigging a stylized/proportioned "
            "mesh risks skinning artifacts. The rigged .glb plus its bundled "
            "basic_animations (walk/run) are RAW AI output (TD-09 quarantine)."
        ),
    )
    src = rig.add_mutually_exclusive_group(required=True)
    src.add_argument(
        "--input-task-id",
        help="rig a prior Meshy generation task by its task id",
    )
    src.add_argument(
        "--model-url",
        help="rig an external mesh by public URL or data URI",
    )
    rig.add_argument(
        "--height-meters",
        type=float,
        default=1.7,
        help="character height in meters (Meshy scales the rig to this; default 1.7)",
    )
    rig.add_argument(
        "--basic-animations-dir",
        type=Path,
        default=None,
        help=(
            "if set, also download the rigging task's bundled basic_animations "
            "(walk/run) clips into this directory (also TD-09 quarantine)."
        ),
    )
    _add_common_meshy_call_args(rig)


def _add_animate_parser(sub) -> None:
    anim = sub.add_parser(
        "animate",
        help="retarget a library action onto a rigged mesh (POST /openapi/v1/animations)",
        epilog=(
            "action_id indexes Meshy's documented animation library "
            "(docs.meshy.ai/en/api/animation). The animation .glb is RAW AI "
            "output (TD-09 quarantine)."
        ),
    )
    anim.add_argument(
        "--rig-task-id",
        required=True,
        help="the completed rigging task id to animate",
    )
    anim.add_argument(
        "--action-id",
        type=int,
        required=True,
        help="the library action id to retarget (e.g. 0=Idle; see the API docs)",
    )
    anim.add_argument(
        "--fps",
        type=int,
        default=None,
        choices=(24, 25, 30, 60),
        help="optional post-process fps for the exported clip",
    )
    _add_common_meshy_call_args(anim)


def _origin_url(task_id: str, endpoint: str) -> str:
    """The Meshy task's canonical API URL — the auditable origin for provenance."""
    return f"{client_mod.BASE_URL}{endpoint}/{task_id}"


def _new_client(
    api_key: str,
    model_version: str = client_mod.DEFAULT_MODEL_VERSION,
    *,
    timeout: float | None = None,
) -> client_mod.MeshyClient:
    """Client construction seam — tests monkeypatch this to inject a mock transport."""
    return client_mod.MeshyClient(api_key, model_version=model_version, timeout=timeout)


def _post_commit_hook() -> None:
    """Test seam for an interrupt after commit and before the terminal event."""


@dataclass
class _TargetReservation:
    """Crash-recoverable ownership of one target and its private staging tree.

    A cross-platform advisory lock provides live-owner exclusion; the OS releases
    it after abnormal process death.  A random sentinel moves *inside* the atomic
    staging-directory rename and must match the external transaction record before
    recovery may delete anything.  The external record's atomic ``committed``
    transition is the irreversible commit point.
    """

    paths: intake.LandingPaths
    token: str
    target_key: str
    target_identity: str
    file_lock: locking.AdvisoryFileLock
    state_path: Path
    staging_dir: Path
    sentinel_path: Path
    partial_glb: Path
    partial_prompts: Path
    partial_sidecar: Path
    committed: bool = False

    _STATE_SCHEMA = "meridian/meshy-job-lock@1"
    _SENTINEL_NAME = ".meshy-ownership.json"

    @staticmethod
    def _lock_root() -> Path:
        override = os.environ.get("MERIDIAN_MESHY_LOCK_DIR")
        if override:
            return Path(override)
        return Path(tempfile.gettempdir()) / "meridian-meshy-locks"

    @staticmethod
    def _identity(paths: intake.LandingPaths) -> str:
        return os.path.normcase(str(paths.asset_dir.resolve(strict=False)))

    @classmethod
    def acquire(cls, paths: intake.LandingPaths) -> _TargetReservation:
        paths.asset_dir.parent.mkdir(parents=True, exist_ok=True)
        identity = cls._identity(paths)
        target_key = hashlib.sha256(identity.encode("utf-8")).hexdigest()
        lock_root = cls._lock_root()
        file_lock = locking.AdvisoryFileLock(lock_root / f"{target_key}.lock")
        try:
            file_lock.acquire()
        except locking.LockUnavailableError as exc:
            raise intake.IntakeError(
                f"output already exists or is reserved under {paths.asset_dir}; "
                "refusing to overwrite an existing asset"
            ) from exc

        state_path = lock_root / f"{target_key}.json"
        staging_dir: Path | None = None
        try:
            cls._recover_stale(paths, state_path, identity)
            if paths.asset_dir.exists():
                raise intake.IntakeError(
                    f"output already exists under {paths.asset_dir}; refusing to "
                    "overwrite an existing asset"
                )

            token = secrets.token_hex(32)
            staging_dir = paths.asset_dir.parent / (
                f".{paths.asset_dir.name}.{token}.partial"
            )
            staging_dir.mkdir()
            sentinel_path = staging_dir / cls._SENTINEL_NAME
            reservation = cls(
                paths=paths,
                token=token,
                target_key=target_key,
                target_identity=identity,
                file_lock=file_lock,
                state_path=state_path,
                staging_dir=staging_dir,
                sentinel_path=sentinel_path,
                partial_glb=staging_dir / paths.glb_path.name,
                partial_prompts=staging_dir / paths.prompts_path.name,
                partial_sidecar=staging_dir / paths.sidecar_path.name,
            )
            reservation._write_sentinel()
            reservation._write_state("staging")
            return reservation
        except BaseException:
            # This process created the random path while holding the target
            # lock, so setup rollback does not need recovery proof.
            if staging_dir is not None and staging_dir.is_dir():
                shutil.rmtree(staging_dir)
            file_lock.release()
            raise

    @classmethod
    def _recover_stale(
        cls, paths: intake.LandingPaths, state_path: Path, identity: str
    ) -> None:
        """Recover only internal sentinels cryptographically bound to state."""

        state = cls._read_document(state_path)
        valid_state = cls._valid_record(state, identity)
        token = state["token"] if valid_state else None
        phase = state["phase"] if valid_state else None
        candidates = list(
            paths.asset_dir.parent.glob(f".{paths.asset_dir.name}.*.partial")
        )

        recoverable_staging = []
        for staging_dir in candidates:
            sentinel = cls._read_document(staging_dir / cls._SENTINEL_NAME)
            if not cls._valid_sentinel(sentinel, identity, token):
                raise intake.IntakeError(
                    f"unrecognized Meshy staging ownership under {staging_dir}; "
                    "refusing unsafe recovery"
                )
            recoverable_staging.append(staging_dir)

        for staging_dir in recoverable_staging:
            shutil.rmtree(staging_dir)

        if paths.asset_dir.is_dir():
            sentinel = cls._read_document(paths.asset_dir / cls._SENTINEL_NAME)
            if phase != "committed" and cls._valid_sentinel(sentinel, identity, token):
                shutil.rmtree(paths.asset_dir)
            elif phase == "committed" and cls._valid_sentinel(
                sentinel, identity, token
            ):
                (paths.asset_dir / cls._SENTINEL_NAME).unlink(missing_ok=True)
            # Every other final directory is legitimate or unprovable and is
            # preserved by the caller's ordinary output-exists refusal.

        if valid_state and phase != "committed" and not paths.asset_dir.exists():
            state_path.unlink(missing_ok=True)

    @classmethod
    def _read_document(cls, path: Path) -> dict:
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError, TypeError):
            return {}
        return document if isinstance(document, dict) else {}

    @classmethod
    def _valid_record(cls, document: dict, identity: str) -> bool:
        token = document.get("token")
        return (
            document.get("schema") == cls._STATE_SCHEMA
            and document.get("target") == identity
            and document.get("phase") in {"staging", "published", "committed"}
            and isinstance(token, str)
            and len(token) == 64
            and all(char in "0123456789abcdef" for char in token)
        )

    @classmethod
    def _valid_sentinel(
        cls, document: dict, identity: str, expected_token: str | None
    ) -> bool:
        token = document.get("token")
        return (
            expected_token is not None
            and document.get("schema") == cls._STATE_SCHEMA
            and document.get("target") == identity
            and isinstance(token, str)
            and secrets.compare_digest(token, expected_token)
        )

    def _document(self, phase: str) -> dict:
        return {
            "schema": self._STATE_SCHEMA,
            "target": self.target_identity,
            "token": self.token,
            "phase": phase,
        }

    def _atomic_write(self, path: Path, document: dict) -> None:
        temporary = path.with_name(f".{path.name}.{self.token}.tmp")
        temporary.write_text(
            json.dumps(document, separators=(",", ":"), sort_keys=True),
            encoding="utf-8",
        )
        temporary.chmod(0o600)
        temporary.replace(path)

    def _write_sentinel(self) -> None:
        self._atomic_write(self.sentinel_path, self._document("staging"))

    def _write_state(self, phase: str) -> None:
        self._atomic_write(self.state_path, self._document(phase))

    def publish(self) -> None:
        """Publish the complete asset as one atomic directory operation."""

        self.staging_dir.replace(self.paths.asset_dir)
        self.sentinel_path = self.paths.asset_dir / self._SENTINEL_NAME
        self._write_state("published")

    def commit(self) -> None:
        """Mark a published asset complete only after client teardown succeeds."""

        self._write_state("committed")
        self.committed = True
        self.sentinel_path.unlink(missing_ok=True)

    def is_committed(self) -> bool:
        state = self._read_document(self.state_path)
        return self._valid_record(state, self.target_identity) and (
            state["token"] == self.token and state["phase"] == "committed"
        )

    def finish_committed(self) -> None:
        """Idempotently finish presentation after the atomic commit point."""

        self.committed = self.is_committed()
        if self.committed:
            (self.paths.asset_dir / self._SENTINEL_NAME).unlink(missing_ok=True)

    def cleanup(self) -> None:
        """Rollback this token's unpublished staging/final output and unlock."""

        try:
            if self.is_committed():
                self.finish_committed()
            else:
                if self.staging_dir.is_dir() and self._valid_sentinel(
                    self._read_document(self.staging_dir / self._SENTINEL_NAME),
                    self.target_identity,
                    self.token,
                ):
                    shutil.rmtree(self.staging_dir)
                elif self.paths.asset_dir.is_dir() and self._valid_sentinel(
                    self._read_document(self.paths.asset_dir / self._SENTINEL_NAME),
                    self.target_identity,
                    self.token,
                ):
                    shutil.rmtree(self.paths.asset_dir)
                self.state_path.unlink(missing_ok=True)
        finally:
            self.file_lock.release()


def cmd_generate(args: argparse.Namespace) -> int:
    api_key = os.environ.get("MESHY_API_KEY")
    events = protocol.EventEmitter(
        secret=api_key, enabled=args.json_events, stream=sys.stdout
    )

    def refuse(message: str) -> int:
        if args.json_events:
            try:
                events.emit("error", error_code="invalid_request", message=message)
            except protocol.ProtocolSinkFlushError as exc:
                # The refusal event is already the terminal outcome. If its
                # accepted line became durable on retry, preserve exit 2 rather
                # than append a second terminal document.
                if exc.retry_succeeded and not exc.is_process_control:
                    return 2
                return _report_json_sink_failure()
        else:
            print(f"refused: {message}", file=sys.stderr)
        return 2

    def emit_preflight(event: str, **fields) -> int | None:
        """Emit an early nonterminal event with runtime-equivalent cancellation."""

        try:
            events.emit(event, **fields)
        except protocol.ProtocolSinkFlushError as exc:
            if not exc.retry_succeeded:
                return _report_json_sink_failure()
            if isinstance(exc.original, KeyboardInterrupt):
                # The interrupted event's full line was accepted. Append only
                # the cancellation terminal; never rewrite the first delivery.
                events.emit(
                    "cancelled",
                    message="generation cancelled by operator",
                    task_ids=[],
                )
                return 130
            if isinstance(exc.original, Exception):
                # The retry proved the sink healthy and made the preflight line
                # durable. Match guarded runtime semantics with one redacted
                # internal-error terminal, without rewriting the first event.
                events.emit(
                    "error",
                    error_code="internal_error",
                    message=str(exc.original),
                    task_ids=[],
                )
                return 1
            return _report_json_sink_failure()
        return None

    # --- Refusal gates: BEFORE any network call (TD-09 precondition). ---
    if (
        not args.model_version
        or args.model_version != args.model_version.strip()
        or args.model_version.casefold() == "latest"
    ):
        return refuse(
            "--model-version must name an explicit pinned Meshy release; "
            "'latest' is refused"
        )
    preflight_result = emit_preflight(
        "validation.started",
        mode="text" if args.text else "image",
        model_version=args.model_version,
    )
    if preflight_result is not None:
        return preflight_result
    if not args.terms_verified:
        return refuse(
            "--terms-verified is required — Meshy's commercial-terms check "
            "must be operator-confirmed before generating (TD-09)"
        )

    if not api_key:
        return refuse("MESHY_API_KEY is not set")

    # Per-request HTTP timeout: CLI --timeout > $MESHY_TIMEOUT > built-in default
    # (issue #735). Resolved here so a malformed value is refused before any
    # network call rather than blowing up mid-generation.
    http_timeout_s = args.timeout
    if http_timeout_s is None:
        env_timeout = os.environ.get("MESHY_TIMEOUT")
        if env_timeout:
            try:
                http_timeout_s = float(env_timeout)
            except ValueError:
                return refuse(
                    f"MESHY_TIMEOUT must be a number of seconds, got {env_timeout!r}"
                )
    if http_timeout_s is not None and http_timeout_s <= 0:
        return refuse(
            f"--timeout must be a positive number of seconds, got {http_timeout_s}"
        )

    try:
        intake.validate_namespace(args.ns)
        intake.validate_name(args.name)
        image_url = intake.image_ref_to_url(args.image) if args.image else None
    except intake.IntakeError as exc:
        return refuse(str(exc))

    # --- Derive the Meshy target_polycount from the class budget (issue #627),
    # still before any network call. Only text-to-3D takes a polycount, so an
    # override paired with --image is refused rather than silently dropped.
    try:
        budgets = intake.load_budgets()
        paths = intake.landing_paths(
            content_root=args.content_root,
            ns=args.ns,
            name=args.name,
            asset_class=args.asset_class,
            budgets=budgets,
        )
        target_polycount: int | None = None
        if args.text:
            target_polycount = intake.derive_target_polycount(
                args.asset_class, budgets, override=args.target_polycount
            )
        elif args.target_polycount is not None:
            return refuse(
                "--target-polycount applies to --text generation only "
                "(image-to-3D does not take a polycount)"
            )
    except (intake.IntakeError, OSError, ValueError) as exc:
        return refuse(str(exc))

    preflight_result = emit_preflight(
        "validation.passed",
        mode="text" if args.text else "image",
        model_version=args.model_version,
    )
    if preflight_result is not None:
        return preflight_result

    task_ids: list[str] = []
    terminal_emitted = False

    def completed() -> int:
        nonlocal terminal_emitted
        if args.json_events and events.has_delivery("completed"):
            events.resume("completed")
            terminal_emitted = True
        if not terminal_emitted:
            asset_id = intake.asset_id(args.ns, args.name)
            if args.json_events:
                events.emit(
                    "completed",
                    asset_id=asset_id,
                    path=str(paths.asset_dir),
                    task_ids=task_ids,
                )
            else:
                print(f"OK — landed {asset_id} at {paths.asset_dir}")
            # Record completion only after the terminal output is flushed.
            # EventEmitter journals the exact serialized line and accepted
            # offset so recovery resumes it rather than writing a duplicate.
            terminal_emitted = True
        return 0

    def on_client_event(event: str, **fields) -> None:
        task_id = fields.get("task_id")
        if task_id and task_id not in task_ids:
            task_ids.append(task_id)
        events.emit(event, model_version=args.model_version, **fields)

    client: client_mod.MeshyClient | None = None
    reservation: _TargetReservation | None = None
    try:
        # Reservation and client construction are deliberately inside this
        # guarded region.  JSON mode must end in a structured terminal event
        # even when local TLS/environment setup fails before the first request.
        reservation = _TargetReservation.acquire(paths)
        client = _new_client(api_key, args.model_version, timeout=http_timeout_s)
        if args.text:
            handle = client.text_to_3d(
                args.text,
                target_polycount=target_polycount,
                poll_interval_s=args.poll_interval,
                poll_timeout_s=args.poll_timeout,
                on_event=on_client_event,
            )
            endpoint = client_mod.TEXT_TO_3D_PATH
            final_stage = "refine"
        else:
            handle = client.image_to_3d(image_url, on_event=on_client_event)
            endpoint = client_mod.IMAGE_TO_3D_PATH
            final_stage = "image"

        status = client.poll(
            handle.task_id,
            endpoint=endpoint,
            interval_s=args.poll_interval,
            timeout_s=args.poll_timeout,
            on_status=lambda state: on_client_event(
                "poll.progress",
                task_id=state.task_id,
                stage=final_stage,
                status=state.status,
                progress=state.progress,
            ),
        )

        if not status.succeeded:
            raise client_mod.MeshyAPIError(
                f"Meshy task {handle.task_id} ended in status {status.status} "
                "(expected SUCCEEDED)",
                task_id=handle.task_id,
                status=status.status,
            )

        events.emit(
            "download.started", task_id=handle.task_id, path=str(paths.glb_path)
        )
        client.download(
            handle.task_id,
            reservation.partial_glb,
            status=status,
            endpoint=endpoint,
        )
        events.emit(
            "download.completed", task_id=handle.task_id, path=str(paths.glb_path)
        )

        # --- Budget pre-check: fail before any sidecar is written. ---
        events.emit("budget.started", path=str(paths.glb_path))
        lod0_tris = intake.count_glb_triangles(reservation.partial_glb)
        precheck_doc = intake.build_budget_precheck_doc(args.asset_class, lod0_tris)
        budget_errors = _check_budget(precheck_doc, paths.sidecar_path)
        if budget_errors:
            message = "budget pre-check failed — " + "; ".join(budget_errors)
            if args.json_events:
                events.emit(
                    "error",
                    error_code="budget_failed",
                    message=message,
                    task_ids=task_ids,
                )
            else:
                print("refused: budget pre-check failed —", file=sys.stderr)
                for err in budget_errors:
                    print(f"  {err}", file=sys.stderr)
            return 1
        events.emit("budget.passed", lod0_tris=lod0_tris)

        # --- Land the prompts file + sidecar (only now that budget passed). ---
        events.emit("provenance.started", asset_id=intake.asset_id(args.ns, args.name))
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

        reservation.partial_prompts.write_text(
            yaml.safe_dump(prompts_doc, sort_keys=False), encoding="utf-8"
        )
        reservation.partial_sidecar.write_text(
            yaml.safe_dump(sidecar_doc, sort_keys=False), encoding="utf-8"
        )
        reservation.publish()
        events.emit(
            "provenance.written",
            asset_id=intake.asset_id(args.ns, args.name),
            path=str(paths.sidecar_path),
        )
        # Teardown is part of a successful job.  If it fails, the generic
        # handler emits one redacted terminal error and reservation cleanup
        # rolls the still-owner-marked atomic publication back for a clean retry.
        client.close()
        client = None
        reservation.commit()
        _post_commit_hook()
        return completed()
    except protocol.ProtocolSinkFlushError as exc:
        if exc.is_process_control:
            return _report_json_sink_failure()
        if isinstance(exc.original, KeyboardInterrupt):
            if reservation is not None and reservation.is_committed():
                reservation.finish_committed()
                return completed()
            if args.json_events:
                events.emit(
                    "cancelled",
                    message="generation cancelled by operator",
                    task_ids=task_ids,
                )
            else:
                print("cancelled: generation interrupted by operator", file=sys.stderr)
            return 130
        if reservation is not None and reservation.is_committed():
            reservation.finish_committed()
            return completed()
        if args.json_events:
            events.emit(
                "error",
                error_code="internal_error",
                message=str(exc.original),
                task_ids=task_ids,
            )
        else:
            print(f"error: {events.redact(str(exc.original))}", file=sys.stderr)
        return 1
    except intake.IntakeError as exc:
        return refuse(str(exc))
    except KeyboardInterrupt:
        if events.sink_poisoned:
            return _report_json_sink_failure()
        if reservation is not None and reservation.is_committed():
            reservation.finish_committed()
            return completed()
        if args.json_events:
            events.emit(
                "cancelled",
                message="generation cancelled by operator",
                task_ids=task_ids,
            )
        else:
            print("cancelled: generation interrupted by operator", file=sys.stderr)
        return 130
    except (client_mod.MeshyAPIError, client_mod.MeshyPollTimeoutError) as exc:
        error_code, http_status = protocol.classify_error(exc)
        if args.json_events:
            fields = {
                "error_code": error_code,
                "message": str(exc),
                "task_ids": task_ids,
            }
            if http_status is not None:
                fields["http_status"] = http_status
            events.emit("error", **fields)
        else:
            print(f"error: {events.redact(str(exc))}", file=sys.stderr)
        return 1
    except Exception as exc:  # noqa: BLE001 — machine protocol must terminate cleanly
        if events.sink_poisoned:
            return _report_json_sink_failure()
        if reservation is not None and reservation.is_committed():
            reservation.finish_committed()
            return completed()
        if args.json_events:
            events.emit(
                "error",
                error_code="internal_error",
                message=str(exc),
                task_ids=task_ids,
            )
        else:
            print(f"error: {events.redact(str(exc))}", file=sys.stderr)
        return 1
    finally:
        if client is not None:
            # A primary terminal error/cancellation already describes the job.
            # Teardown must never replace it or create a second terminal event.
            with contextlib.suppress(Exception):
                client.close()
        if reservation is not None:
            reservation.cleanup()


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


def _resolve_call_timeout_s(raw: float | None) -> float | None:
    """CLI --timeout > $MESHY_TIMEOUT > default; raise ValueError on a bad value.

    Shared by rig/animate. Mirrors cmd_generate's precedence so a malformed
    timeout is rejected before any network call rather than mid-generation.
    """
    if raw is None:
        env_timeout = os.environ.get("MESHY_TIMEOUT")
        if env_timeout:
            try:
                raw = float(env_timeout)
            except ValueError:
                raise ValueError(
                    f"MESHY_TIMEOUT must be a number of seconds, got {env_timeout!r}"
                )
    if raw is not None and raw <= 0:
        raise ValueError(f"--timeout must be a positive number of seconds, got {raw}")
    return raw


def _preflight_live_call(args: argparse.Namespace) -> tuple[str, float | None] | int:
    """Shared rig/animate TD-09 gate: pinned version, --terms-verified, api key.

    Returns ``(api_key, http_timeout_s)`` on success, or an exit code (2) on
    refusal. The MESHY_API_KEY value is read from the environment and NEVER
    printed — only its presence is reported.
    """
    if (
        not args.model_version
        or args.model_version != args.model_version.strip()
        or args.model_version.casefold() == "latest"
    ):
        print(
            "refused: --model-version must name an explicit pinned Meshy release; "
            "'latest' is refused",
            file=sys.stderr,
        )
        return 2
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
        http_timeout_s = _resolve_call_timeout_s(args.timeout)
    except ValueError as exc:
        print(f"refused: {exc}", file=sys.stderr)
        return 2
    return api_key, http_timeout_s


def cmd_rig(args: argparse.Namespace) -> int:
    gate = _preflight_live_call(args)
    if isinstance(gate, int):
        return gate
    api_key, http_timeout_s = gate

    try:
        client = _new_client(
            api_key, model_version=args.model_version, timeout=http_timeout_s
        )
    except ValueError as exc:
        print(f"refused: {exc}", file=sys.stderr)
        return 2

    try:
        with client:
            handle = client.rig(
                input_task_id=args.input_task_id,
                model_url=args.model_url,
                height_meters=args.height_meters,
            )
            status = client.poll(
                handle.task_id,
                endpoint=client_mod.RIGGING_PATH,
                interval_s=args.poll_interval,
                timeout_s=args.poll_timeout,
            )
            if not status.succeeded:
                print(
                    f"error: rigging task {handle.task_id} ended in "
                    f"{status.status}, expected SUCCEEDED",
                    file=sys.stderr,
                )
                return 1
            glb_url = client.rigged_glb_url(status)
            if not glb_url:
                print(
                    f"error: rigging task {handle.task_id} SUCCEEDED but no "
                    "rigged_character_glb_url in result "
                    f"(result keys: {sorted(status.result)})",
                    file=sys.stderr,
                )
                return 1
            client.download_url(glb_url, args.out)
            basic = client.basic_animation_urls(status)
            downloaded_basic: list[str] = []
            if args.basic_animations_dir and basic:
                for clip_key, clip_url in sorted(basic.items()):
                    if not clip_key.endswith("_glb_url") or not clip_url:
                        continue
                    name = clip_key[: -len("_glb_url")]
                    dest = args.basic_animations_dir / f"{name}.glb"
                    client.download_url(clip_url, dest)
                    downloaded_basic.append(str(dest))
    except (client_mod.MeshyAPIError, client_mod.MeshyPollTimeoutError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    summary = {
        "task_id": handle.task_id,
        "status": status.status,
        "rigged_glb": str(args.out),
        "result_keys": sorted(status.result),
        "basic_animations_keys": sorted(basic),
        "basic_animations_downloaded": downloaded_basic,
    }
    print(json.dumps(summary, indent=2, sort_keys=True))
    print(
        "note: RAW AI output (TD-09 quarantine) — do NOT stage under "
        "content/<ns>/assets/ as mergeable pack content.",
        file=sys.stderr,
    )
    return 0


def cmd_animate(args: argparse.Namespace) -> int:
    gate = _preflight_live_call(args)
    if isinstance(gate, int):
        return gate
    api_key, http_timeout_s = gate

    try:
        client = _new_client(
            api_key, model_version=args.model_version, timeout=http_timeout_s
        )
    except ValueError as exc:
        print(f"refused: {exc}", file=sys.stderr)
        return 2

    post_process = {"fps": args.fps} if args.fps is not None else None
    try:
        with client:
            handle = client.animate(
                args.rig_task_id, args.action_id, post_process=post_process
            )
            status = client.poll(
                handle.task_id,
                endpoint=client_mod.ANIMATION_PATH,
                interval_s=args.poll_interval,
                timeout_s=args.poll_timeout,
            )
            if not status.succeeded:
                print(
                    f"error: animation task {handle.task_id} ended in "
                    f"{status.status}, expected SUCCEEDED",
                    file=sys.stderr,
                )
                return 1
            glb_url = client.animation_glb_url(status)
            if not glb_url:
                print(
                    f"error: animation task {handle.task_id} SUCCEEDED but no "
                    "animation_glb_url in result "
                    f"(result keys: {sorted(status.result)})",
                    file=sys.stderr,
                )
                return 1
            client.download_url(glb_url, args.out)
    except (client_mod.MeshyAPIError, client_mod.MeshyPollTimeoutError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    summary = {
        "task_id": handle.task_id,
        "status": status.status,
        "action_id": args.action_id,
        "animation_glb": str(args.out),
        "result_keys": sorted(status.result),
    }
    print(json.dumps(summary, indent=2, sort_keys=True))
    print(
        "note: RAW AI output (TD-09 quarantine) — do NOT stage under "
        "content/<ns>/assets/ as mergeable pack content.",
        file=sys.stderr,
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
    effective_argv = list(argv) if argv is not None else sys.argv[1:]
    parser = _build_parser()
    if "--json-events" in effective_argv:
        parse_stderr = io.StringIO()
        try:
            with contextlib.redirect_stderr(parse_stderr):
                args = parser.parse_args(effective_argv)
        except SystemExit as exc:
            if exc.code == 0:
                return 0
            emitter = protocol.EventEmitter(secret=os.environ.get("MESHY_API_KEY"))
            try:
                emitter.emit(
                    "error",
                    error_code="invalid_request",
                    message=parse_stderr.getvalue().strip()
                    or "invalid command arguments",
                )
            except protocol.ProtocolSinkFlushError as exc:
                if exc.retry_succeeded and not exc.is_process_control:
                    return 2
                return _report_json_sink_failure()
            except protocol.ProtocolSinkError:
                return _report_json_sink_failure()
            return 2
    else:
        args = parser.parse_args(effective_argv)
    try:
        if args.command == "generate":
            return cmd_generate(args)
        if args.command == "convert-rig":
            return cmd_convert_rig(args)
        if args.command == "rig":
            return cmd_rig(args)
        if args.command == "animate":
            return cmd_animate(args)
    except protocol.ProtocolSinkFlushError:
        return _report_json_sink_failure()
    except protocol.ProtocolSinkError:
        return _report_json_sink_failure()
    parser.error(f"unknown command: {args.command}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
