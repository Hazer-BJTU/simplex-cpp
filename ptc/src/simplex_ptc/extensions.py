"""Explicit trusted-host extension registration; no production tools are bundled."""

import hashlib
import importlib
import inspect
import keyword
from collections.abc import Callable
from dataclasses import asdict, dataclass

from .protocol import PTCError, encode


@dataclass(frozen=True)
class ToolDescriptor:
    """Stable manifest entry. Bump version when callable semantics change.

    Signature and effects are documentation, not permission grants. Extensions
    are trusted host Python code, outside Monty's sandbox.
    """

    name: str
    description: str
    signature: str
    result: str
    effects: str
    version: str


class ToolRegistry:
    """One deterministic registry serves discovery, binding and compatibility."""

    def __init__(self):
        self._tools: dict[str, tuple[ToolDescriptor, Callable]] = {}

    def add(self, descriptor: ToolDescriptor, handler: Callable) -> None:
        """Register a synchronous plain-data callable; reject ambiguous names."""
        if (
            not isinstance(descriptor, ToolDescriptor)
            or not all(isinstance(v, str) and v for v in asdict(descriptor).values())
            or not descriptor.name.isidentifier()
            or keyword.iskeyword(descriptor.name)
            or descriptor.name.startswith("_")
            or descriptor.name in self._tools
            or not callable(handler)
            or inspect.iscoroutinefunction(handler)
        ):
            raise PTCError("extension_error", "invalid or duplicate tool declaration")
        self._tools[descriptor.name] = descriptor, handler

    def manifest(self) -> list[dict]:
        """Return sorted descriptors without importing arbitrary working-directory code."""
        return [asdict(self._tools[name][0]) for name in sorted(self._tools)]

    def fingerprint(self) -> str:
        """Fingerprint declared compatibility, not mutable Python function addresses."""
        return hashlib.sha256(encode(self.manifest())).hexdigest()

    def bindings(self) -> dict[str, Callable]:
        """Bind plain JSON-only values; never leak live host objects into snapshots."""

        def wrap(handler):
            def invoke(*args, **kwargs):
                encode([args, kwargs])
                result = handler(*args, **kwargs)
                encode(result)
                return result

            return invoke

        return {name: wrap(handler) for name, (_, handler) in self._tools.items()}

    @classmethod
    def load(cls, modules: list[str]) -> "ToolRegistry":
        """Load only operator-selected installed modules exporting register_tools."""
        registry = cls()
        for name in modules:
            try:
                importlib.import_module(name).register_tools(registry)
            except Exception as exc:
                raise PTCError("extension_error", f"cannot register extension {name}") from exc
        return registry
