"""Versioned JSON-lines events for machine orchestration of Meshy intake.

The protocol intentionally carries no API key, authorization header, or raw
provider response.  ``EventEmitter`` also applies defense-in-depth recursive
redaction before serializing every event.
"""

from __future__ import annotations

import json
import re
import sys
from dataclasses import dataclass
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


class ProtocolSinkError(RuntimeError):
    """Base error for a JSON event sink that can no longer be used safely."""


class ProtocolSinkContractError(ProtocolSinkError):
    """The sink violated the supported ``TextIO.write`` return contract."""


class ProtocolSinkAmbiguousWriteError(ProtocolSinkError):
    """A write raised, leaving its accepted character count unknowable."""


@dataclass
class _Delivery:
    """Resumable delivery state for one exact serialized event document."""

    event: str
    document: dict[str, Any]
    line: str
    offset: int = 0
    flushed: bool = False


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
        self._deliveries: list[_Delivery] = []
        self._sink_poisoned = False

    @property
    def sink_poisoned(self) -> bool:
        """Whether stdout delivery failed with an unknowable safe continuation."""

        return self._sink_poisoned

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
        if self._enabled and self._sink_poisoned:
            raise ProtocolSinkError(
                "Meshy JSON event sink is poisoned; refusing further output"
            )
        document = self.build(event, **fields)
        if self._enabled:
            line = json.dumps(document, separators=(",", ":"), sort_keys=True) + "\n"
            delivery = _Delivery(event=event, document=document, line=line)
            # Journal the exact line before its first byte is offered.  A
            # short write or interruption can therefore resume this document
            # and sequence rather than serializing a second event onto a prefix.
            self._deliveries.append(delivery)
            self._advance(delivery)
        return document

    def was_emitted(self, event: str) -> bool:
        """Whether a complete line for this event has been accepted."""

        return any(
            delivery.event == event and delivery.offset == len(delivery.line)
            for delivery in self._deliveries
        )

    def has_delivery(self, event: str) -> bool:
        """Whether an exact serialized delivery exists for this event."""

        return any(delivery.event == event for delivery in self._deliveries)

    def resume(self, event: str) -> dict[str, Any]:
        """Resume the latest unflushed delivery for ``event`` idempotently."""

        if self._enabled and self._sink_poisoned:
            raise ProtocolSinkError(
                "Meshy JSON event sink is poisoned; refusing further output"
            )

        for delivery in reversed(self._deliveries):
            if delivery.event == event:
                self._advance(delivery)
                return delivery.document
        raise ValueError(f"no Meshy protocol delivery exists for {event!r}")

    def ensure_flushed(self, event: str) -> None:
        """Compatibility wrapper: resume the latest event through flush."""

        self.resume(event)

    def _advance(self, delivery: _Delivery) -> None:
        """Write all remaining characters, then flush exactly once successfully."""

        while delivery.offset < len(delivery.line):
            remaining = delivery.line[delivery.offset :]
            try:
                count = self._stream.write(remaining)
            except BaseException as exc:
                # A custom sink may accept a prefix and raise in the same call.
                # Its side effects are unobservable, so no continuation offset
                # can be chosen without risking duplicate or malformed JSON.
                self._sink_poisoned = True
                raise ProtocolSinkAmbiguousWriteError(
                    "Meshy JSON event sink write failed with an indeterminate "
                    "accepted offset"
                ) from exc
            if (
                isinstance(count, bool)
                or not isinstance(count, int)
                or count <= 0
                or count > len(remaining)
            ):
                self._sink_poisoned = True
                raise ProtocolSinkContractError(
                    "Meshy JSON event sink returned an invalid write count"
                )
            delivery.offset += count
        if not delivery.flushed:
            self._stream.flush()
            delivery.flushed = True

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
