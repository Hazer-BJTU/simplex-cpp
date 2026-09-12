# The tools module's tests. Three translation units, one per header, all
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
#   test_toolsets          ToolInterface / ToolSet::handle(): the happy path,
#                          every checkpoint's failure record and stage, the
#                          query correlation a failure record must keep, and the
#                          end-to-end confirmation of a RequireConfirm tool.
#
# test_security_check and test_toolsets link the async event bus through
# tools_iface (async_eventbus_iface). Both drive coroutines on a local
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
