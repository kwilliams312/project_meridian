"""Versioned JSON-lines events for machine orchestration of Meshy intake.

The protocol intentionally carries no API key, authorization header, or raw
provider response.  ``EventEmitter`` also applies defense-in-depth recursive
redaction before serializing every event.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Any, TextIO

PROTOCOL_SCHEMA = "meridian/meshy-job-event@1"
PROTOCOL_VERSION = 1
SCHEMA_PATH = Path(__file__).with_name("job-event.schema.json")

EVENT_NAMES = (
    "validation.started",
    "validation.passed",
    "preview.submitted",
    "refine.submitted",
    "generation.submitted",
    "poll.progress",
    "download.started",
    "download.completed",
    "budget.started",
    "budget.passed",
    "provenance.started",
    "provenance.written",
    "completed",
    "cancelled",
    "error",
)

_BEARER_RE = re.compile(r"(?i)(authorization\s*:\s*)?bearer\s+[^\s,;\"']+")


class EventEmitter:
    """Build and optionally emit deterministic, one-object-per-line events."""

    def __init__(
        self,
        *,
        secret: str | None = None,
        stream: TextIO | None = None,
        enabled: bool = True,
    ) -> None:
        self._secret = secret
        self._stream = stream if stream is not None else sys.stdout
        self._enabled = enabled
        self._sequence = 0
        self._emitted_events: set[str] = set()

    def build(self, event: str, **fields: Any) -> dict[str, Any]:
        if event not in EVENT_NAMES:
            raise ValueError(f"unknown Meshy protocol event {event!r}")
        self._sequence += 1
        document = {
            "schema": PROTOCOL_SCHEMA,
            "protocol_version": PROTOCOL_VERSION,
            "sequence": self._sequence,
            "event": event,
            **fields,
        }
        return self.redact(document)

    def emit(self, event: str, **fields: Any) -> dict[str, Any]:
        document = self.build(event, **fields)
        if self._enabled:
            print(
                json.dumps(document, separators=(",", ":"), sort_keys=True),
                file=self._stream,
                flush=True,
            )
            self._emitted_events.add(event)
        return document

    def was_emitted(self, event: str) -> bool:
        """Whether this emitter successfully wrote at least one such event."""

        return event in self._emitted_events

    def redact(self, value: Any) -> Any:
        if isinstance(value, dict):
            return {key: self.redact(item) for key, item in value.items()}
        if isinstance(value, list):
            return [self.redact(item) for item in value]
        if isinstance(value, tuple):
            return [self.redact(item) for item in value]
        if not isinstance(value, str):
            return value
        result = value
        if self._secret:
            result = result.replace(self._secret, "[REDACTED]")
        return _BEARER_RE.sub("[REDACTED]", result)


def classify_error(exc: BaseException) -> tuple[str, int | None]:
    """Return the stable protocol error code and optional provider HTTP code."""

    # Avoid importing client at module load (keeps this protocol module pure).
    status_code = getattr(exc, "status_code", None)
    if status_code == 402:
        return "payment_required", status_code
    if status_code == 429:
        return "rate_limited", status_code
    if exc.__class__.__name__ == "MeshyPollTimeoutError":
        return "timeout", None
    if "unrecognized status" in str(exc):
        return "status_drift", status_code
    return "provider_error", status_code
