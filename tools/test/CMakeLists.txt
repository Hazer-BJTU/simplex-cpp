# The tools module's tests. Four translation units, one per header, all
# in-process (no sockets, no child processes):
#
#   test_invoke_exception  the failure type and its bridge to the model_io
#                          contract: the rendered failure (what()/to_string()),
#                          the InvokeReturn it converts into (prose in
#                          output.raw, failure marker in extras), the marker
#                          read back live and after a JSON round trip, and the
#                          detached-invocation handler that returns the failure
#                          as its result.
#   test_security_check    the default invocation security policy: Trusted and
#                          DefaultDeny decided without the bus, RequireConfirm
#                          answered through an InvokeConfirmEvent by handlers
#                          subscribed on a bus the test owns, and the fail-
#                          closed cases (no subscriber, no answer, a throwing
#                          handler).
#   test_toolsets          ToolInterface / ToolSet's two phases: phase 1
#                          (prepare) settling a call in place and resolving its
#                          tool, phase 2 (execute) running the settled call to
#                          its record, every checkpoint's failure and stage, the
#                          query correlation a record must keep, and what the
#                          split is for — settling a batch before scheduling it,
#                          by query.type.
#   test_registry          ToolRegistry: the routing table (registration, the
#                          duplicate refusal, removal, the flattened catalogue)
#                          and a whole batch of calls — routed and settled
#                          first, serial before parallel, joined, and assembled
#                          back into one record per call in call order. The
#                          cases a batch has to survive are all here: an
#                          unknown tool, a set that breaks either of its two
#                          contracts, a refused call, a failed tool, and two
#                          calls sharing an identity. Every batch runs under a
#                          watchdog, so a collector left waiting for a report
#                          fails the case instead of hanging the suite.
#
# test_security_check, test_toolsets and test_registry link the async event bus
# through tools_iface (async_eventbus_iface). Both drive coroutines on a local
# io_context; the one case that exercises default_async_bus() disconnects its
# subscription before it ends, so the process-wide bus is left clean.
function(tools_add_test name)
    add_executable(${name} ${name}.cpp)
    target_compile_options(${name} PRIVATE
        $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
    )
    target_link_libraries(${name} PRIVATE
        tools_iface
        Boost::unit_test_framework
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

tools_add_test(test_invoke_exception)
tools_add_test(test_security_check)
tools_add_test(test_toolsets)
tools_add_test(test_registry)
# One case drives a batch of parallel calls on a four-thread io_context — the
# case that makes the registry's report channel a concurrent_channel — so this
# test needs pthread: the plain compile option, matching utils/eventbus/test and
# llm/test's concurrency test, which preserves the per-config option sets above.
target_compile_options(test_registry PRIVATE -pthread)
