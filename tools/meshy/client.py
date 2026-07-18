"""Thin httpx wrapper around the Meshy.ai REST API (spec §7.1).

Deliberately dumb: this module knows how to start a generation task, poll it,
and download the finished ``.glb`` — nothing else. All policy (TD-09 refusal
gates, sidecar shaping, budget pre-check) lives in ``intake.py`` /
``__main__.py`` so it can be unit-tested without touching HTTP at all.

Endpoint shapes are from the public Meshy API docs (docs.meshy.ai, fetched
2026-07-10):

  * Text-to-3D is a two-stage flow on the same endpoint
    (``POST /openapi/v2/text-to-3d``): a ``mode: preview`` call generates the
    untextured mesh, then a ``mode: refine`` call (keyed by
    ``preview_task_id``) textures it. Both return ``{"result": "<task_id>"}``.
  * Image-to-3D is single-stage (``POST /openapi/v1/image-to-3d``), same
    response shape.
  * Both task families are polled at ``GET <path>/{task_id}``, returning
    ``status`` (``PENDING|IN_PROGRESS|SUCCEEDED|FAILED|CANCELED``),
    ``progress``, and — once ``SUCCEEDED`` — ``model_urls`` (a dict with a
    ``glb`` key holding a presigned download URL).

CI never calls this module against the live API (all HTTP is mocked in
tests/test_meshy.py) and the CLI never runs it without an operator-confirmed
``--terms-verified`` flag (TD-09).
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

import httpx

# --- API surface constants -----------------------------------------------
# Base host + versioned paths, per the public docs. v1/v2 mismatch between
# text-to-3d and image-to-3d is a real quirk of the Meshy API, not a typo.
BASE_URL = "https://api.meshy.ai"
TEXT_TO_3D_PATH = "/openapi/v2/text-to-3d"
IMAGE_TO_3D_PATH = "/openapi/v1/image-to-3d"
# Rigging + animation live on v1 (docs.meshy.ai/en/api/rigging + /animation,
# fetched 2026-07-17). Unlike text/image-to-3d, their SUCCEEDED bodies do NOT
# carry a top-level ``model_urls`` map: the finished glb URLs are nested under a
# ``result`` object keyed by asset kind — ``result.rigged_character_glb_url`` /
# ``result.animation_glb_url`` / ``result.basic_animations.<clip>_glb_url``.
# download() (which keys off model_urls["glb"]) therefore does NOT work for these
# tasks; use download_url() with a URL pulled from TaskStatus.result instead.
RIGGING_PATH = "/openapi/v1/rigging"
ANIMATION_PATH = "/openapi/v1/animations"

# Pinned model version — recorded verbatim into sidecar `provenance.ai.tool`
# as `meshy@<model-ver>` (spec §7.2). Pin a specific release, never "latest":
# provenance must name an exact, reproducible tool version (TD-09).
DEFAULT_MODEL_VERSION = "meshy-5"

DEFAULT_TARGET_POLYCOUNT = 30_000
DEFAULT_POLL_INTERVAL_S = 5.0
DEFAULT_POLL_TIMEOUT_S = 600.0

# --- Per-request HTTP timeout (issue #735) --------------------------------
# The old code hardwired a single 30s httpx timeout, which killed slow-but-
# healthy Meshy calls: create/refine POSTs and the presigned .glb download can
# legitimately take well over a minute (SP5 Wave 3 lost 3/7 pieces to a
# transient 30s read-timeout while 4/7 succeeded in the same run). Keep a short
# connect cap — a slow *response* is the failure mode, never a slow TCP handshake
# — but a generous read/write/pool so the client waits the response out instead
# of aborting it. This per-request budget is DISTINCT from DEFAULT_POLL_TIMEOUT_S,
# which caps the overall PENDING→terminal poll loop; the two must not be conflated.
DEFAULT_HTTP_CONNECT_TIMEOUT_S = 10.0
DEFAULT_HTTP_TIMEOUT_S = 120.0
DEFAULT_HTTP_TIMEOUT = httpx.Timeout(
    DEFAULT_HTTP_TIMEOUT_S, connect=DEFAULT_HTTP_CONNECT_TIMEOUT_S
)

TERMINAL_STATUSES = frozenset({"SUCCEEDED", "FAILED", "CANCELED"})
PENDING_STATUSES = frozenset({"PENDING", "IN_PROGRESS"})
# The full documented status vocabulary (docs.meshy.ai). Anything outside this
# set means the API shape has drifted from what this client was written
# against — poll() fails loudly and immediately rather than spinning until a
# generic timeout that would mask the real problem.
KNOWN_STATUSES = TERMINAL_STATUSES | PENDING_STATUSES


def resolve_http_timeout(timeout: "httpx.Timeout | float | None") -> httpx.Timeout:
    """Normalize a caller-supplied per-request timeout into an httpx.Timeout.

    ``None`` → the built-in default (short connect, generous read/write/pool).
    An ``httpx.Timeout`` is honored verbatim. A scalar (seconds) raises the
    read/write/pool budget to that value but keeps the short connect cap — a
    slow connect is never the failure mode issue #735 is about, and letting the
    connect balloon to minutes would just hide a genuinely unreachable host.
    """
    if timeout is None:
        return DEFAULT_HTTP_TIMEOUT
    if isinstance(timeout, httpx.Timeout):
        return timeout
    seconds = float(timeout)
    return httpx.Timeout(seconds, connect=min(DEFAULT_HTTP_CONNECT_TIMEOUT_S, seconds))


class MeshyAPIError(RuntimeError):
    """A Meshy API call returned an error (non-2xx, or a terminal-but-failed task)."""

    def __init__(
        self,
        message: str,
        *,
        task_id: str | None = None,
        status: str | None = None,
        status_code: int | None = None,
    ):
        super().__init__(message)
        self.task_id = task_id
        self.status = status
        self.status_code = status_code


class MeshyPollTimeoutError(RuntimeError):
    """A task did not reach a terminal status within the polling budget."""

    def __init__(self, task_id: str, timeout_s: float):
        super().__init__(f"Meshy task {task_id} did not finish within {timeout_s:.0f}s")
        self.task_id = task_id


@dataclass
class TaskHandle:
    """A submitted generation task, plus enough context to audit it later.

    ``request_payload`` is the exact JSON body of the request that *started
    the generation* (the preview request for text-to-3d, the image request
    for image-to-3d) — this is what lands verbatim in the prompts file
    (Art PRD §3.2 prompt hygiene). ``task_id`` is the task whose completion
    the caller should poll/download (the refine task for text-to-3d).
    """

    task_id: str
    mode: str  # "text_refine" | "image" | "rig" | "animate"
    request_payload: dict
    preview_task_id: str | None = None  # set only when mode == "text_refine"


@dataclass
class TaskStatus:
    task_id: str
    status: str
    progress: int = 0
    model_urls: dict = field(default_factory=dict)
    # ``result`` holds the SUCCEEDED body's ``result`` object when it is a dict
    # (rigging/animation tasks nest their finished-asset URLs here). Text/image
    # tasks never populate it — their poll bodies carry ``model_urls`` instead
    # (their *create* response's scalar ``result`` is a task id, not this).
    result: dict = field(default_factory=dict)
    raw: dict = field(default_factory=dict)

    @property
    def succeeded(self) -> bool:
        return self.status == "SUCCEEDED"

    @property
    def terminal(self) -> bool:
        return self.status in TERMINAL_STATUSES


class MeshyClient:
    """Bearer-authenticated client for the Meshy generation API."""

    def __init__(
        self,
        api_key: str,
        *,
        base_url: str = BASE_URL,
        model_version: str = DEFAULT_MODEL_VERSION,
        transport: httpx.BaseTransport | None = None,
        timeout: "httpx.Timeout | float | None" = None,
    ):
        if not api_key:
            raise ValueError("MeshyClient requires a non-empty api_key")
        if (
            not model_version
            or model_version != model_version.strip()
            or model_version.casefold() == "latest"
        ):
            raise ValueError(
                "MeshyClient requires a pinned model version; 'latest' is refused"
            )
        self.model_version = model_version
        self.timeout = resolve_http_timeout(timeout)
        self._client = httpx.Client(
            base_url=base_url.rstrip("/"),
            headers={"Authorization": f"Bearer {api_key}"},
            transport=transport,
            timeout=self.timeout,
        )

    def close(self) -> None:
        self._client.close()

    def __enter__(self) -> "MeshyClient":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    # --- Task creation -----------------------------------------------------

    def text_to_3d(
        self,
        prompt: str,
        *,
        target_polycount: int = DEFAULT_TARGET_POLYCOUNT,
        enable_pbr: bool = False,
        poll_interval_s: float = DEFAULT_POLL_INTERVAL_S,
        poll_timeout_s: float = DEFAULT_POLL_TIMEOUT_S,
        sleep: Callable[[float], None] = time.sleep,
        now: Callable[[], float] = time.monotonic,
        on_event: Callable[..., Any] | None = None,
    ) -> TaskHandle:
        """Run the preview stage to completion, then submit the refine stage.

        Returns a `TaskHandle` for the REFINE task (the one whose `.glb` is
        the finished, textured asset) — the caller polls/downloads that.
        """
        preview_payload = {
            "mode": "preview",
            "prompt": prompt,
            "ai_model": self.model_version,
            "target_polycount": target_polycount,
        }
        preview_task_id = self._create_task(TEXT_TO_3D_PATH, preview_payload)
        if on_event:
            on_event("preview.submitted", task_id=preview_task_id, stage="preview")
        preview_status = self.poll(
            preview_task_id,
            endpoint=TEXT_TO_3D_PATH,
            interval_s=poll_interval_s,
            timeout_s=poll_timeout_s,
            sleep=sleep,
            now=now,
            on_status=(
                lambda status: (
                    on_event(
                        "poll.progress",
                        task_id=status.task_id,
                        stage="preview",
                        status=status.status,
                        progress=status.progress,
                    )
                    if on_event
                    else None
                )
            ),
        )
        if not preview_status.succeeded:
            raise MeshyAPIError(
                f"preview task {preview_task_id} ended in status "
                f"{preview_status.status}, expected SUCCEEDED",
                task_id=preview_task_id,
                status=preview_status.status,
            )
        refine_payload = {
            "mode": "refine",
            "preview_task_id": preview_task_id,
            "ai_model": self.model_version,
            "enable_pbr": enable_pbr,
        }
        refine_task_id = self._create_task(TEXT_TO_3D_PATH, refine_payload)
        if on_event:
            on_event("refine.submitted", task_id=refine_task_id, stage="refine")
        return TaskHandle(
            task_id=refine_task_id,
            mode="text_refine",
            request_payload=preview_payload,
            preview_task_id=preview_task_id,
        )

    def image_to_3d(
        self, image_url: str, *, on_event: Callable[..., Any] | None = None
    ) -> TaskHandle:
        payload = {"image_url": image_url, "ai_model": self.model_version}
        task_id = self._create_task(IMAGE_TO_3D_PATH, payload)
        if on_event:
            on_event("generation.submitted", task_id=task_id, stage="image")
        return TaskHandle(task_id=task_id, mode="image", request_payload=payload)

    def rig(
        self,
        input_task_id: str | None = None,
        *,
        model_url: str | None = None,
        height_meters: float = 1.7,
        texture_image_url: str | None = None,
        on_event: Callable[..., Any] | None = None,
    ) -> TaskHandle:
        """Submit an auto-rigging task (``POST /openapi/v1/rigging``).

        Rig either a prior generation task (``input_task_id``) or an external
        mesh (``model_url`` — a public URL or data URI); exactly one is
        required. Returns a ``TaskHandle`` whose ``task_id`` is polled at
        ``endpoint=RIGGING_PATH``; the SUCCEEDED body nests the rigged glb URL
        (and a ``basic_animations`` walk/run bundle) under ``result`` — pull it
        with ``rigged_glb_url``/``basic_animation_urls`` and fetch via
        ``download_url`` (NOT ``download``).
        """
        if bool(input_task_id) == bool(model_url):
            raise ValueError("rig() requires exactly one of input_task_id or model_url")
        payload: dict[str, Any] = {"height_meters": height_meters}
        if input_task_id:
            payload["input_task_id"] = input_task_id
        else:
            payload["model_url"] = model_url
        if texture_image_url:
            payload["texture_image_url"] = texture_image_url
        task_id = self._create_task(RIGGING_PATH, payload)
        if on_event:
            on_event("rigging.submitted", task_id=task_id, stage="rig")
        return TaskHandle(task_id=task_id, mode="rig", request_payload=payload)

    def animate(
        self,
        rig_task_id: str,
        action_id: int,
        *,
        post_process: dict | None = None,
        on_event: Callable[..., Any] | None = None,
    ) -> TaskHandle:
        """Submit an animation task (``POST /openapi/v1/animations``).

        Retargets a documented library ``action_id`` (see
        docs.meshy.ai/en/api/animation) onto a completed rigging task. Returns a
        ``TaskHandle`` polled at ``endpoint=ANIMATION_PATH``; the SUCCEEDED body
        nests ``result.animation_glb_url`` — pull it with ``animation_glb_url``
        and fetch via ``download_url``.
        """
        payload: dict[str, Any] = {
            "rig_task_id": rig_task_id,
            "action_id": action_id,
        }
        if post_process is not None:
            payload["post_process"] = post_process
        task_id = self._create_task(ANIMATION_PATH, payload)
        if on_event:
            on_event("animation.submitted", task_id=task_id, stage="animate")
        return TaskHandle(task_id=task_id, mode="animate", request_payload=payload)

    def _create_task(self, path: str, payload: dict) -> str:
        resp = self._client.post(path, json=payload)
        self._raise_for_http_error(resp)
        data = resp.json()
        task_id = data.get("result") or data.get("id")
        if not task_id:
            raise MeshyAPIError(f"Meshy response missing a task id: {data}")
        return task_id

    # --- Polling -------------------------------------------------------

    def poll(
        self,
        task_id: str,
        *,
        endpoint: str = TEXT_TO_3D_PATH,
        interval_s: float = DEFAULT_POLL_INTERVAL_S,
        timeout_s: float = DEFAULT_POLL_TIMEOUT_S,
        sleep: Callable[[float], None] = time.sleep,
        now: Callable[[], float] = time.monotonic,
        on_status: Callable[[TaskStatus], Any] | None = None,
    ) -> TaskStatus:
        """Poll a task until it reaches a terminal status, or time out.

        `sleep`/`now` are injectable so tests can drive a fake clock instead
        of sleeping for real (repo testing rules: no arbitrary real waits).
        A status value outside KNOWN_STATUSES (including a missing `status`
        field) raises MeshyAPIError immediately — API-shape drift must fail
        loudly, not dribble into a generic poll timeout.
        """
        deadline = now() + timeout_s
        while True:
            resp = self._client.get(f"{endpoint}/{task_id}")
            self._raise_for_http_error(resp)
            data = resp.json()
            raw_status = data.get("status")
            if raw_status not in KNOWN_STATUSES:
                raise MeshyAPIError(
                    f"Meshy task {task_id} returned unrecognized status "
                    f"{raw_status!r} — expected one of "
                    f"{', '.join(sorted(KNOWN_STATUSES))}. The API shape may "
                    f"have drifted from what this client was written against "
                    f"(see tools/meshy/README.md 'API notes').",
                    task_id=task_id,
                    status=raw_status,
                )
            result = data.get("result")
            status = TaskStatus(
                task_id=task_id,
                status=raw_status,
                progress=data.get("progress", 0),
                model_urls=data.get("model_urls") or {},
                result=result if isinstance(result, dict) else {},
                raw=data,
            )
            if on_status:
                on_status(status)
            if status.terminal:
                return status
            if now() >= deadline:
                raise MeshyPollTimeoutError(task_id, timeout_s)
            sleep(interval_s)

    # --- Download --------------------------------------------------------

    def download(
        self,
        task_id: str,
        dest: Path,
        *,
        status: TaskStatus | None = None,
        endpoint: str = TEXT_TO_3D_PATH,
    ) -> Path:
        """Fetch the finished task's `.glb` to `dest`. Polls to completion if
        `status` isn't already supplied (e.g. by a caller that just polled)."""
        if status is None:
            status = self.poll(task_id, endpoint=endpoint)
        if not status.succeeded:
            raise MeshyAPIError(
                f"task {task_id} is not SUCCEEDED (status={status.status}) — cannot download",
                task_id=task_id,
                status=status.status,
            )
        glb_url = status.model_urls.get("glb")
        if not glb_url:
            raise MeshyAPIError(
                f"task {task_id} succeeded but model_urls has no 'glb' entry",
                task_id=task_id,
                status=status.status,
            )
        resp = self._client.get(glb_url)
        self._raise_for_http_error(resp)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(resp.content)
        return dest

    def download_url(self, url: str, dest: Path) -> Path:
        """Fetch an arbitrary presigned asset URL to ``dest``.

        The URL-keyed download variant for rigging/animation results, whose
        finished glbs live at ``result.*_glb_url`` rather than the
        ``model_urls["glb"]`` slot ``download`` assumes. The caller supplies the
        URL (e.g. via ``rigged_glb_url``/``animation_glb_url``/
        ``basic_animation_urls``); this only fetches and writes bytes.
        """
        if not url:
            raise MeshyAPIError("download_url requires a non-empty URL")
        resp = self._client.get(url)
        self._raise_for_http_error(resp)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(resp.content)
        return dest

    # --- Rigging/animation result-URL accessors --------------------------
    # These key off TaskStatus.result (NOT model_urls) — the shape rigging and
    # animation SUCCEEDED bodies actually use. Kept as thin, named readers so
    # the CLI/spike never hard-codes the nested key strings inline.

    @staticmethod
    def rigged_glb_url(status: TaskStatus) -> str | None:
        """The rigged-character glb URL from a SUCCEEDED rigging task."""
        return status.result.get("rigged_character_glb_url")

    @staticmethod
    def basic_animation_urls(status: TaskStatus) -> dict:
        """The rigging task's bundled ``basic_animations`` clip URL map (walk/run)."""
        bundle = status.result.get("basic_animations")
        return bundle if isinstance(bundle, dict) else {}

    @staticmethod
    def animation_glb_url(status: TaskStatus) -> str | None:
        """The animation glb URL from a SUCCEEDED animation task."""
        return status.result.get("animation_glb_url")

    @staticmethod
    def _raise_for_http_error(resp: httpx.Response) -> None:
        if resp.status_code >= 400:
            raise MeshyAPIError(
                f"Meshy API returned {resp.status_code} for {resp.request.url}: {resp.text[:500]}",
                status_code=resp.status_code,
            )
