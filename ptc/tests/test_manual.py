"""Real Monty coverage of the manual's flat host-object API and persistence limits."""

from dataclasses import replace

import pytest
from pydantic_monty import ClassInstance

from simplex_ptc import PTCError, Request, Session, ToolDescriptor, ToolRegistry
from simplex_ptc.manual import ManualResult
from simplex_ptc.protocol import MAX_RESPONSE_BYTES, encode


def run(session, source):
    """Run a fresh request against this test's current session epoch."""
    info = session.inspect()
    return session.execute(Request(str(info["revision"]), source, info["epoch"]))


def test_directory_is_typed_list_and_does_not_print_or_call_tools(tmp_path):
    registry = ToolRegistry()
    calls = []
    registry.add(
        ToolDescriptor("zebra", "Example function.", "zebra() -> int", "int", "none", "1"),
        lambda: calls.append(True),
    )
    result = run(Session(tmp_path, registry), "manual()")
    assert [entry["name"] for entry in result["result"]] == ["manual", "zebra"]
    assert result["result"][1]["summary"] == "Example function."
    assert result["result"][1]["signature"] == "zebra() -> int"
    assert result["output"] == ""
    assert calls == []


@pytest.mark.parametrize(
    "source, multiple",
    [
        ("manual()", True),
        ('manual("manual")', False),
        ('manual(["manual"])', True),
        ('manual("manual", "manual")', True),
        ('manual(["manual", "manual"])', True),
    ],
)
def test_result_objects_are_automatically_detached_at_output(tmp_path, source, multiple):
    result = run(Session(tmp_path), source)
    assert result["status"] == "completed", result
    values = result["result"] if multiple else [result["result"]]
    for value in values:
        assert value["name"] == "manual"
        assert value["signature"].startswith("manual(")
        assert value["inputs"] and value["returns"] and value["examples"]
        assert value["return_attributes"] and value["return_methods"]
        assert all(isinstance(item, (str, list)) for item in value.values())


def test_member_access_methods_and_printing(tmp_path):
    result = run(
        Session(tmp_path),
        """m = manual("manual")
print(m.render())
[m.name, m.signature, m.to_dict()["name"], m.return_methods]
""",
    )
    assert result["status"] == "completed", result
    assert result["result"][0] == "manual"
    assert result["result"][2] == "manual"
    for section in ("Inputs:", "Returns:", "Return attributes:", "Return methods:", "Examples:"):
        assert section in result["output"]


def test_batch_order_empty_list_and_nested_final_output(tmp_path):
    registry = ToolRegistry()
    registry.add(
        ToolDescriptor("sample", "Sample.", "sample() -> int", "int", "none", "1"), lambda: 1
    )
    result = run(
        Session(tmp_path, registry),
        '{"batch": manual("sample", "manual", "sample"), "empty": manual([])}',
    )
    assert [item["name"] for item in result["result"]["batch"]] == ["sample", "manual", "sample"]
    assert result["result"]["empty"] == []


@pytest.mark.parametrize(
    "expression, error",
    [
        ('manual("missing")', "ValueError"),
        ('manual(["manual", "missing"])', "ValueError"),
        ("manual(1)", "TypeError"),
        ('manual(["manual"], "manual")', "TypeError"),
        ('manual(["manual", 1])', "TypeError"),
        ('manual(name="manual")', "TypeError"),
    ],
)
def test_invalid_queries_can_be_caught_in_sandbox(tmp_path, expression, error):
    result = run(
        Session(tmp_path), f"try:\n    {expression}\nexcept {error}:\n    caught = True\ncaught"
    )
    assert result["result"] is True, result


def test_unexposed_members_are_not_accessible(tmp_path):
    result = run(
        Session(tmp_path),
        """m = manual("manual")
try:
    m.__getattribute__("name")
except AttributeError:
    denied = True
denied
""",
    )
    assert result["result"] is True, result


def test_plain_manual_data_survives_restore_and_fresh_query_works(tmp_path):
    session = Session(tmp_path)
    first = run(session, 'm = manual("manual")\nsaved = m.to_dict()\nsaved')
    assert first["status"] == "completed", first
    second = run(session, '[saved, manual("manual").to_dict()]')
    assert second["status"] == "completed", second
    assert second["result"] == [first["result"], first["result"]]
    third = run(
        session,
        """try:
    m.render()
except Exception:
    unavailable = True
unavailable""",
    )
    assert third["result"] is True, third


def test_arbitrary_host_objects_are_still_rejected():
    registry = ToolRegistry()
    registry.add(
        ToolDescriptor("bad", "Bad.", "bad()", "object", "none", "1"),
        lambda: ClassInstance(object()),
    )
    with pytest.raises(TypeError):
        registry.bindings()["bad"]()


def test_manual_cannot_be_overridden_and_details_are_validated():
    registry = ToolRegistry()
    descriptor = ToolDescriptor("manual", "Override.", "manual()", "str", "none", "1")
    with pytest.raises(PTCError):
        registry.add(descriptor, lambda: "")
    with pytest.raises(PTCError):
        registry.add(replace(descriptor, name="bad", inputs=({"name": "nested"},)), lambda: "")


def test_to_dict_returns_detached_data():
    value = ToolRegistry().bindings()["manual"]("manual").value
    assert type(value) is ManualResult
    first = value.to_dict()
    first["examples"].clear()
    assert value.to_dict()["examples"]


def test_large_manual_keeps_bounded_valid_response(tmp_path):
    registry = ToolRegistry()
    registry.add(
        ToolDescriptor(
            "large", "Large docs.", "large() -> int", "int", "none", "1", details="x" * 60000
        ),
        lambda: 1,
    )
    result = run(Session(tmp_path, registry), 'manual("large")')
    assert result["status"] == "completed", result
    assert result["result_truncated"]
    assert len(encode(result)) <= MAX_RESPONSE_BYTES


def test_documentation_changes_invalidate_registry_fingerprint():
    first, second = ToolRegistry(), ToolRegistry()
    descriptor = ToolDescriptor("sample", "Sample.", "sample() -> int", "int", "none", "1")
    first.add(descriptor, lambda: 1)
    second.add(replace(descriptor, inputs=("changed description",)), lambda: 1)
    assert first.fingerprint() != second.fingerprint()


def test_restored_host_proxy_is_not_silently_serialized(tmp_path):
    session = Session(tmp_path)
    first = run(session, 'info = manual("manual")\ninfo.to_dict()')
    assert first["status"] == "completed"
    restored = run(session, "info")
    assert restored["status"] == "failed"
    assert restored["error"]["code"] == "conversion_error"
    assert restored["revision"] == first["revision"]


def test_complete_extension_description_uses_single_registration(tmp_path):
    registry = ToolRegistry()
    descriptor = ToolDescriptor(
        "sample",
        "Sample summary.",
        "sample(count: int = 2) -> SampleResult",
        "SampleResult: exposes only the following data and methods.",
        "none",
        "1",
        details="Detailed sample behavior.",
        inputs=("count: int = 2; must be positive.",),
        return_attributes=("items: list[str] — selected items.",),
        return_methods=("to_dict() -> dict: detached plain data.",),
        raises=("ValueError: count is not positive.",),
        notes=("Host methods are invocation-local.",),
        examples=("sample(count=2).to_dict()",),
    )
    registry.add(descriptor, lambda count=2: None)
    result = run(Session(tmp_path, registry), 'manual("sample").to_dict()')
    assert result["result"]["signature"] == descriptor.signature
    assert result["result"]["inputs"] == list(descriptor.inputs)
    assert result["result"]["return_methods"] == list(descriptor.return_methods)
    assert result["result"]["examples"] == list(descriptor.examples)


def test_directory_entries_support_members_methods_and_print_example(tmp_path):
    registry = ToolRegistry.load(["simplex_ptc.tools.process"])
    result = run(
        Session(tmp_path, registry),
        """for info in manual():
    print(f"{info.name}: {info.summary}")
[info.to_dict() for info in manual()]
""",
    )
    assert result["status"] == "completed", result
    assert result["output"].splitlines() == [
        "manual: List available functions or describe exact function names.",
        "run_command: Execute a shell command; kill or detach when waiting expires.",
    ]
    assert [entry["name"] for entry in result["result"]] == ["manual", "run_command"]
    assert result["result"][1]["return_methods"]


def test_registry_error_hook_covers_boundaries_and_keeps_original_failure():
    calls = []
    registry = ToolRegistry()
    descriptor = ToolDescriptor("guarded", "Guarded.", "guarded(value) -> int", "int", "none", "1")
    registry.add(descriptor, lambda value: value, on_error=lambda: calls.append("discard"))
    function = registry.bindings()["guarded"]
    with pytest.raises(TypeError):
        function()
    with pytest.raises(TypeError):
        function({1})
    with pytest.raises(ValueError):
        function(float("nan"))
    assert calls == ["discard"] * 3
    assert function(1) == 1
    assert calls == ["discard"] * 3

    def cleanup_failure():
        raise OSError("cleanup failed")

    registry.add(
        replace(descriptor, name="bad_result"), lambda value: object(), on_error=cleanup_failure
    )
    with pytest.raises(TypeError) as error:
        registry.bindings()["bad_result"](1)
    assert any("cleanup failed" in note for note in error.value.__notes__)
