"""Independent Draft 2020-12 checks; install test/requirements.txt first.

The trailing-marker keyword adds ordering that Draft 2020-12 cannot express
for an arbitrary-length array. Standard keywords still check item shapes and
marker count; protocol-aware validators additionally enforce the final slot.
"""
import json
from pathlib import Path
import unittest
from jsonschema import Draft202012Validator, ValidationError, validators


def trailing_marker(validator, key, instance, schema):
    if isinstance(instance, list):
        for item in instance[:-1]:
            if isinstance(item, dict) and key in item:
                yield ValidationError("omission marker must be the final element")


Validator = validators.extend(
    Draft202012Validator, {"x-simplex-trailing-marker": trailing_marker}
)
SCHEMA = json.loads((Path(__file__).parents[1] / "schemas/worker-protocol.json").read_text())["json_schema"]
SOURCE = dict(worker_id="w", turn=0, step=0, commit_sequence="1", fingerprint="a" * 64)
CALL = dict(id="c", name="tool", arguments=None, type="read_only", security="trusted")
MARKER = dict(display_omitted=True, omitted_items=2)


class SchemaTest(unittest.TestCase):
    def check(self, record, value, valid=True):
        schema = dict(SCHEMA, **{"$ref": "#/$defs/" + record})
        self.assertEqual(Validator(schema).is_valid(value), valid, repr(value))

    def test_payload_operation_scope(self):
        self.check("PayloadRequest", dict(operation="answer", request_id="q", source=SOURCE, part=0, offset=0))
        self.check("PayloadRequest", dict(operation="answer", request_id="q", source=SOURCE, unknown=True), False)
        message = dict(operation="message", request_id="q", content=[dict(type="text", modality="text", raw="x")], start="ignored", source={})
        self.check("PayloadRequest", message)
        self.check("PayloadRequest", dict(operation="history", request_id="q", source=42))
        self.check("PayloadRequest", dict(operation="history", request_id="q", start="bad"), False)
        self.check("PayloadRequest", dict(operation="message", request_id="q"), False)
        self.check("PayloadRequest", dict(operation="continue", request_id="q", content=[]), False)
        self.check("PayloadRequest", dict(message, options={"confirmation": {"mode": "future"}, "other": 42}))

    def test_tool_response(self):
        base = dict(worker_id="w", session_id="s", run_id="r", request_id="q", route="future/route")
        error = dict(code="new_code", message="reason")
        for status in ("succeeded", "rejected"):
            for data, valid in (({}, False), ({"result": {}, "error": error}, False),
                                ({"result": {}}, status == "succeeded"),
                                ({"error": error}, status == "rejected")):
                reply = dict(base, status=status, **data)
                self.check("ToolResponse", reply, valid)
                self.check("ToolResponseEnvelope", dict(type="tool_response", data=reply), valid)

    def test_compact_union(self):
        for summary, valid in ((42, False), ("text", True),
                               (dict(display_omitted=True, bytes=10, reason="omitted"), True), ({}, False)):
            self.check("CompactFinishedData", dict(summary=summary, removed_turns=0, revision=0, durable=True), valid)

    def test_projected_batch(self):
        for calls, valid in ((42, False), ([], True), ([CALL], True),
                             ([CALL, MARKER], True), ([MARKER, CALL], False),
                             ([MARKER, MARKER], False), ([{}], False)):
            self.check("ResponseProjection", dict(index=0, type="model_response", role="assistant", content=[], tool_calls=0, omitted_parts=0, invokes=calls), valid)

    def test_schema_is_well_formed(self):
        Draft202012Validator.check_schema(SCHEMA)


if __name__ == "__main__":
    unittest.main()
