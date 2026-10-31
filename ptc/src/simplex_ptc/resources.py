"""Invocation-local ownership for host file editors; never part of a Monty dump."""

from contextlib import contextmanager
from contextvars import ContextVar

MAX_OPEN_FILES = 16
MAX_STAGED_BYTES = 1024 * 1024
_current = ContextVar("ptc_resources", default=None)


class ExecutionResources:
    """Bound live editors and replacement data; close every editor on exit.

    A failed file operation invalidates all still-open editors in this invocation.
    Already committed files cannot be rolled back. New editors may be opened
    after a caught error, but invalidated objects cannot be reused.
    """

    def __init__(self):
        self.files = set()
        self.staged_bytes = 0

    def reserve(self, size: int) -> None:
        """Reserve aggregate replacement bytes before modifying an edit plan."""
        if self.staged_bytes + size > MAX_STAGED_BYTES:
            raise ValueError("invocation staged replacement budget exceeded")
        self.staged_bytes += size

    def abort(self) -> None:
        """Best-effort closure of every editor, preserving the first cleanup error."""
        failure = None
        for file in tuple(self.files):
            try:
                file.close()
            except BaseException as exc:
                if failure is None:
                    failure = exc
        if failure is not None:
            raise failure


def current_resources() -> ExecutionResources:
    """Require an execution scope instead of silently leaking host file handles."""
    value = _current.get()
    if value is None:
        raise RuntimeError("open_text requires an active PTC execution scope")
    return value


@contextmanager
def execution_resources():
    """Release uncommitted work before the caller can publish a Python checkpoint."""
    resources = ExecutionResources()
    token = _current.set(resources)
    try:
        try:
            yield resources
        except BaseException as exc:
            try:
                resources.abort()
            except BaseException as cleanup:
                exc.add_note(f"file cleanup also failed: {cleanup}")
            raise
        else:
            resources.abort()
    finally:
        _current.reset(token)


def abort_file_operations() -> None:
    """Invalidate active file work, including failures before a handler is entered.

    Used by registry boundary cleanup and the direct Python entry wrapper. No
    active scope means there are no editors to invalidate; keep the original
    argument error instead of replacing it with a missing-scope error.
    """
    scope = _current.get()
    if scope is not None:
        scope.abort()
