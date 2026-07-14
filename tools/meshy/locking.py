"""Cross-platform non-blocking advisory file locks for Meshy jobs.

Platform modules are imported lazily so importing ``meshy`` and ``convert-rig``
works on Windows (where :mod:`fcntl` does not exist) as well as POSIX hosts.
"""

from __future__ import annotations

import os
from pathlib import Path
from types import ModuleType
from typing import IO


class LockUnavailableError(RuntimeError):
    """The lock is currently held by another live process."""


class AdvisoryFileLock:
    """One-byte non-blocking file lock backed by fcntl or msvcrt."""

    def __init__(
        self,
        path: Path,
        *,
        platform: str | None = None,
        fcntl_module: ModuleType | None = None,
        msvcrt_module: ModuleType | None = None,
    ) -> None:
        self.path = path
        self._platform = platform or os.name
        self._fcntl = fcntl_module
        self._msvcrt = msvcrt_module
        self._handle: IO[bytes] | None = None

    def acquire(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        handle = self.path.open("a+b")
        handle.seek(0, os.SEEK_END)
        if handle.tell() == 0:
            handle.write(b"\0")
            handle.flush()
        handle.seek(0)
        try:
            if self._platform == "nt":
                backend = self._msvcrt
                if backend is None:
                    import msvcrt as backend  # type: ignore[import-not-found]

                backend.locking(handle.fileno(), backend.LK_NBLCK, 1)
            else:
                backend = self._fcntl
                if backend is None:
                    import fcntl as backend  # type: ignore[import-not-found]

                backend.flock(handle.fileno(), backend.LOCK_EX | backend.LOCK_NB)
        except OSError as exc:
            handle.close()
            raise LockUnavailableError(str(exc)) from exc
        self._handle = handle

    def release(self) -> None:
        handle = self._handle
        if handle is None:
            return
        try:
            handle.seek(0)
            if self._platform == "nt":
                backend = self._msvcrt
                if backend is None:
                    import msvcrt as backend  # type: ignore[import-not-found]

                backend.locking(handle.fileno(), backend.LK_UNLCK, 1)
            else:
                backend = self._fcntl
                if backend is None:
                    import fcntl as backend  # type: ignore[import-not-found]

                backend.flock(handle.fileno(), backend.LOCK_UN)
        finally:
            handle.close()
            self._handle = None

    def __enter__(self) -> AdvisoryFileLock:
        self.acquire()
        return self

    def __exit__(self, *_exc_info) -> None:
        self.release()
