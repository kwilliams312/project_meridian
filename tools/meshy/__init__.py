"""Meshy.ai intake CLI — AI-asset generation with TD-09 provenance automation.

See ``tools/meshy/README.md`` for usage. ``client.py`` is a thin httpx wrapper
around the Meshy API; ``intake.py`` holds the pure sidecar/prompts-file shaping
logic (no network I/O); ``__main__.py`` wires both into ``python -m meshy``.
"""

from __future__ import annotations
