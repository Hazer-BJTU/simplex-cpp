"""Read-only function discovery and explicitly exposed, flat host result objects."""

from dataclasses import asdict, dataclass
from typing import TYPE_CHECKING

from pydantic_monty import ClassInstance

if TYPE_CHECKING:
    from .extensions import ToolRegistry


@dataclass(frozen=True)
class ManualResult:
    """Complete documentation for one function, not a directory or search page.

    All description fields are strings or sequences of strings. Only this outer
    object is a host proxy. Call to_dict() before retaining documentation across
    interpreter invocations: host methods are not restored from Monty snapshots.
    """

    name: str
    summary: str
    signature: str
    description: str
    inputs: tuple[str, ...]
    returns: str
    return_attributes: tuple[str, ...]
    return_methods: tuple[str, ...]
    raises: tuple[str, ...]
    effects: str
    notes: tuple[str, ...]
    examples: tuple[str, ...]

    def to_dict(self) -> dict:
        """Return detached JSON-compatible data with no remaining host proxies."""
        return {
            key: list(value) if isinstance(value, tuple) else value
            for key, value in asdict(self).items()
        }

    def render(self) -> str:
        """Render the full signature, inputs, result API and runnable examples."""
        lines = [self.signature, self.summary, "", self.description]
        for title, entries in (
            ("Inputs", self.inputs),
            ("Returns", (self.returns,)),
            ("Return attributes", self.return_attributes),
            ("Return methods", self.return_methods),
            ("Raises", self.raises),
            ("Effects", (self.effects,)),
            ("Notes", self.notes),
            ("Examples", self.examples),
        ):
            if entries:
                lines.extend(["", f"{title}:"])
                for entry in entries:
                    lines.extend(f"    {line}" for line in entry.splitlines())
        return "\n".join(lines)


def wrap_manual(value: ManualResult) -> ClassInstance:
    """Expose only documented data and two read-only methods, never all methods."""
    return ClassInstance(
        value,
        eager_attrs=tuple(ManualResult.__dataclass_fields__),
        allowed_methods={"to_dict", "render"},
    )


def manual(registry: "ToolRegistry", *names: object) -> str | ClassInstance | list[ClassInstance]:
    """List names or describe exact names; validate a batch before producing it."""
    if not names:
        return "\n".join(
            f"{entry['name']}: {entry['description']}" for entry in registry.manifest()
        )
    single = len(names) == 1 and isinstance(names[0], str)
    requested = names[0] if len(names) == 1 and isinstance(names[0], list) else names
    if not all(isinstance(name, str) for name in requested):
        raise TypeError("manual expects a name, a list of names, or positional names")
    available = {entry["name"]: entry for entry in registry.manifest()}
    missing = [name for name in requested if name not in available]
    if missing:
        raise ValueError(f"unknown function names: {', '.join(missing)}")
    results = []
    for name in requested:
        entry = available[name]
        results.append(
            wrap_manual(
                ManualResult(
                    name=name,
                    summary=entry["description"],
                    signature=entry["signature"],
                    description=entry["details"] or entry["description"],
                    inputs=entry["inputs"],
                    returns=entry["result"],
                    return_attributes=entry["return_attributes"],
                    return_methods=entry["return_methods"],
                    raises=entry["raises"],
                    effects=entry["effects"],
                    notes=entry["notes"],
                    examples=entry["examples"],
                )
            )
        )
    return results[0] if single else results


def register_manual(registry: "ToolRegistry") -> None:
    """Install the always-available manual from the same registry it describes."""
    from .extensions import ToolDescriptor

    registry.add(
        ToolDescriptor(
            name="manual",
            description="List available functions or describe exact function names.",
            signature="manual(*names: str | list[str]) -> str | ManualResult | list[ManualResult]",
            result="No arguments: str. One string: ManualResult. A list or multiple strings: list[ManualResult].",
            effects="Read-only registry lookup; does not invoke described functions or import modules.",
            version="1",
            details="Lists the current registry in name order. Exact queries preserve input order and duplicates. An empty list returns [].",
            inputs=(
                "names: omit for a compact directory; pass one string, one list[str], or multiple strings. Do not mix lists with additional arguments.",
            ),
            return_attributes=(
                "name: str — registered function name.",
                "summary: str — short description.",
                "signature: str — complete Python-style signature, including defaults and return types.",
                "description: str — detailed behavior.",
                "inputs: tuple[str, ...] — parameter types, defaults, meanings and constraints.",
                "returns: str — return types and outcome semantics.",
                "return_attributes: tuple[str, ...] — exposed result properties and their types.",
                "return_methods: tuple[str, ...] — exposed result method signatures and behavior.",
                "raises: tuple[str, ...] — possible exceptions and causes.",
                "effects: str — side effects and permissions boundary.",
                "notes: tuple[str, ...] — limitations and lifecycle notes.",
                "examples: tuple[str, ...] — Python usage examples.",
            ),
            return_methods=(
                "to_dict() -> dict: detached plain data; sequence fields become lists.",
                "render() -> str: full formatted documentation; display with print(result.render()).",
            ),
            raises=(
                "TypeError: invalid argument types or mixed list/positional form.",
                "ValueError: any requested function is unknown; no partial result is returned.",
            ),
            notes=(
                "ManualResult is an explicitly wrapped host ClassInstance, not a sandbox-native class.",
                "Host methods are invocation-local. Persist to_dict() or render() instead; query manual again after restore.",
                "A fresh ManualResult used as the final result is converted to plain data automatically, including inside lists/dicts.",
            ),
            examples=(
                "print(manual())",
                'info = manual("manual")\nprint(info.render())\ninfo.to_dict()',
                'manual(["manual"])',
                'manual("manual", "manual")',
            ),
        ),
        lambda *names: manual(registry, *names),
    )
