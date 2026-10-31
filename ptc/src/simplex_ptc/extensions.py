"""Explicit trusted-host extensions with always-available read-only documentation."""

import builtins
import hashlib
import importlib
import inspect
import keyword
from collections.abc import Callable
from dataclasses import asdict, dataclass

from .protocol import PTCError, encode

# Conservatively reserve Python's entire builtin namespace, including names the
# pinned Monty runtime does not yet implement. external_lookup only handles
# unresolved names, so these cannot reliably refer to registered host functions.
RESERVED_NAMES = frozenset(vars(builtins))


@dataclass(frozen=True)
class ToolDescriptor:
    """Stable manifest entry. Bump version when callable semantics change.

    Signature and effects are documentation, not permission grants. Extensions
    are trusted host Python code, outside Monty's sandbox. Documentation uses
    flat strings and tuples of strings; no nested descriptor/proxy graph is
    required. Include defaults in signature, input constraints in inputs, and
    the exposed ClassInstance API in return_attributes and return_methods.
    Existing plain-value extensions may omit the optional detail fields.
    """

    name: str
    description: str
    signature: str
    result: str
    effects: str
    version: str
    details: str = ""
    inputs: tuple[str, ...] = ()
    return_attributes: tuple[str, ...] = ()
    return_methods: tuple[str, ...] = ()
    raises: tuple[str, ...] = ()
    notes: tuple[str, ...] = ()
    examples: tuple[str, ...] = ()


class ToolRegistry:
    """One deterministic registry serves discovery, binding and compatibility."""

    def __init__(self):
        self._tools: dict[str, tuple[ToolDescriptor, Callable]] = {}
        self._error_handlers: dict[str, Callable | None] = {}
        from .manual import register_manual

        register_manual(self)

    def add(
        self,
        descriptor: ToolDescriptor,
        handler: Callable,
        *,
        on_error: Callable[[], None] | None = None,
    ) -> None:
        """Register a callable and optional invocation-failure cleanup.

        on_error covers boundary conversion, Python argument binding, handler
        execution and result validation. It must be synchronous and must not
        publish effects; its failure is attached without hiding the original.
        """
        if (
            not isinstance(descriptor, ToolDescriptor)
            or not all(
                isinstance(getattr(descriptor, key), str) and getattr(descriptor, key)
                for key in ("name", "description", "signature", "result", "effects", "version")
            )
            or not isinstance(descriptor.details, str)
            or not all(
                type(getattr(descriptor, key)) is tuple
                and all(isinstance(item, str) and item for item in getattr(descriptor, key))
                for key in (
                    "inputs",
                    "return_attributes",
                    "return_methods",
                    "raises",
                    "notes",
                    "examples",
                )
            )
            or not descriptor.name.isidentifier()
            or keyword.iskeyword(descriptor.name)
            or descriptor.name.startswith("_")
            or descriptor.name in self._tools
            or not callable(handler)
            or inspect.iscoroutinefunction(handler)
            or (
                on_error is not None
                and (not callable(on_error) or inspect.iscoroutinefunction(on_error))
            )
        ):
            raise PTCError("extension_error", "invalid or duplicate tool declaration")
        if descriptor.name in RESERVED_NAMES:
            raise PTCError(
                "extension_error", f"tool name {descriptor.name!r} is reserved for Python builtins"
            )
        self._tools[descriptor.name] = descriptor, handler
        self._error_handlers[descriptor.name] = on_error

    def manifest(self) -> list[dict]:
        """Return sorted descriptors without importing arbitrary working-directory code."""
        return [asdict(self._tools[name][0]) for name in sorted(self._tools)]

    def fingerprint(self) -> str:
        """Fingerprint declared compatibility, not mutable Python function addresses."""
        return hashlib.sha256(encode(self.manifest())).hexdigest()

    def bindings(self) -> dict[str, Callable]:
        """Bind plain arguments and explicitly supported host result wrappers."""

        def wrap(handler, on_error):
            def invoke(*args, **kwargs):
                try:
                    encode([args, kwargs])
                    result = handler(*args, **kwargs)
                    from .results import validate_host_result

                    validate_host_result(result)
                    return result
                except BaseException as exc:
                    if on_error is not None:
                        try:
                            on_error()
                        except BaseException as cleanup:
                            exc.add_note(f"tool failure cleanup also failed: {cleanup}")
                    raise

            return invoke

        return {
            name: wrap(handler, self._error_handlers[name])
            for name, (_, handler) in self._tools.items()
        }

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
