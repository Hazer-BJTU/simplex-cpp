"""Explicit result conversion; never call arbitrary user objects' to_dict methods."""

from pydantic_monty import ClassInstance

from .manual import ManualResult
from .protocol import encode
from .tools.files import CommitResult, EditPreview, TextFile, TextSlice
from .tools.process import CommandResult


def plain_result(value: object, depth: int = 0) -> object:
    """Detach known live result objects recursively before final JSON conversion.

    Restored host proxies are deliberately not accepted: the original instance
    is gone. Persist the plain to_dict result instead. Re-querying read-only
    manual data is safe; never replay a shell command just to recreate its object.
    """
    if depth > 128:
        raise ValueError("result exceeds nesting limit")
    if type(value) in (ManualResult, CommandResult, TextSlice, EditPreview, CommitResult):
        return value.to_dict()
    if type(value) in (list, tuple):
        return [plain_result(item, depth + 1) for item in value]
    if type(value) is dict:
        return {key: plain_result(item, depth + 1) for key, item in value.items()}
    return value


def validate_host_result(value: object, depth: int = 0) -> None:
    """Allow only the explicit built-in result wrappers in addition to plain data.

    Validation does not widen method policies or automatically wrap host objects.
    Future built-in result classes need an explicit addition here.
    """
    if depth > 128:
        raise ValueError("result exceeds nesting limit")
    if type(value) is ClassInstance and type(value.value) is TextFile:
        # This handle is only callable in the active resource scope; it cannot
        # be serialized as a final result or revived by a session dump.
        return
    if type(value) is ClassInstance and type(value.value) in (
        ManualResult,
        CommandResult,
        TextSlice,
        EditPreview,
        CommitResult,
    ):
        encode(value.value.to_dict())
    elif type(value) in (list, tuple):
        for item in value:
            validate_host_result(item, depth + 1)
    elif type(value) is dict and all(type(key) is str for key in value):
        for item in value.values():
            validate_host_result(item, depth + 1)
    else:
        encode(value)
