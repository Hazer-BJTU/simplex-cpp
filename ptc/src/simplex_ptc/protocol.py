"""Small, bounded JSON contract shared by the CLI and session engine."""

import json
from dataclasses import dataclass

PROTOCOL_VERSION = 1
MAX_REQUEST_BYTES = 1024 * 1024
MAX_RESPONSE_BYTES = 48 * 1024
MAX_OUTPUT_BYTES = 16 * 1024
MAX_CHECKPOINT_BYTES = 32 * 1024 * 1024


class PTCError(Exception):
    """An actionable boundary error; its code is stable for CLI consumers."""

    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def encode(value: object) -> bytes:
    """Encode strict JSON, rejecting non-finite numbers and unsupported objects."""

    def check(item: object, depth: int = 0) -> None:
        if depth > 128:
            raise ValueError("JSON value exceeds nesting limit")
        if type(item) in (str, int, float, bool, type(None)):
            return
        if type(item) in (list, tuple):
            for child in item:
                check(child, depth + 1)
            return
        if type(item) is dict and all(type(key) is str for key in item):
            for child in item.values():
                check(child, depth + 1)
            return
        raise TypeError("expected plain JSON values and string object keys")

    check(value)
    return json.dumps(value, ensure_ascii=True, allow_nan=False, separators=(",", ":")).encode()


def preview(text: str, limit: int) -> str:
    """Return a UTF-8 prefix without splitting a code point."""
    return text.encode("utf-8", errors="replace")[:limit].decode("utf-8", errors="ignore")


@dataclass(frozen=True)
class Request:
    """One caller-identified execution. IDs must be unique within an epoch."""

    execution_id: str
    source: str
    epoch: str | None = None

    @classmethod
    def parse(cls, value: object) -> "Request":
        """Reject ambiguous requests before creating any session state."""
        if (
            not isinstance(value, dict)
            or not {"execution_id", "source"} <= set(value)
            or set(value) - {"execution_id", "source", "epoch"}
        ):
            raise PTCError("invalid_request", "expected execution_id and source")
        identity, source = value["execution_id"], value["source"]
        if not isinstance(identity, str) or not 1 <= len(identity) <= 128:
            raise PTCError("invalid_request", "execution_id must contain 1–128 characters")
        if not isinstance(source, str):
            raise PTCError("invalid_request", "source must be a string")
        epoch = value.get("epoch")
        if epoch is not None and (not isinstance(epoch, str) or len(epoch) != 36):
            raise PTCError("invalid_request", "epoch must be a session UUID")
        try:
            source.encode("utf-8")
        except UnicodeError as exc:
            raise PTCError("invalid_request", "source must be valid UTF-8") from exc
        request = cls(identity, source, epoch)
        if len(encode(value)) > MAX_REQUEST_BYTES:
            raise PTCError("invalid_request", "request exceeds byte limit")
        return request


class Output:
    """Bound captured print output while continuing to drain runtime messages."""

    def __init__(self):
        self.text = ""
        self.omitted_bytes = 0

    def write(self, text: str) -> None:
        raw = text.encode("utf-8", errors="replace")
        available = MAX_OUTPUT_BYTES - len(self.text.encode())
        kept = raw[:available].decode("utf-8", errors="ignore")
        self.text += kept
        self.omitted_bytes += len(raw) - len(kept.encode())


def bound_response(response: dict) -> dict:
    """Keep complete small results; represent oversized results as explicit previews.

    Budget the final serialized envelope, including JSON escaping and metadata.
    This function runs before committing a successful interpreter state.
    """
    try:
        raw = encode(response)
        if len(raw) <= MAX_RESPONSE_BYTES:
            return response
        result = encode(response.get("result"))
    except (TypeError, ValueError, RecursionError) as exc:
        raise PTCError("conversion_error", "result is not a supported JSON value") from exc
    # Preserve small final results even when print escaping consumes the budget.
    if len(result) > MAX_RESPONSE_BYTES // 2:
        response["result"] = None
        response["result_preview"] = preview(result.decode(), 2048)
        response["result_truncated"] = True
    while len(encode(response)) > MAX_RESPONSE_BYTES:
        output = response.get("output", "")
        if not output:
            raise PTCError("conversion_error", "response metadata exceeds budget")
        shortened = output[: len(output) // 2]
        response["output_omitted_bytes"] += len(output.encode()) - len(shortened.encode())
        response["output"] = shortened
    return response
