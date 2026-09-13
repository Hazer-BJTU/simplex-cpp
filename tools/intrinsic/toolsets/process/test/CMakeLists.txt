# Toolset-local tests for the process family. Two translation units, both
# driving real (but harmless) children — echo / cat / sleep / true / seq, the
# same coreutils process/'s own suite uses:
#
#   test_session_store  the session table on its own: id minting and recycling,
#                       the delta/full read distinction and its per-stream
#                       cursors, the deadline override, the session cap, waiting
#                       with and without a deadline, the refusal to release a
#                       live child, and both shutdown paths (terminate_all and
#                       the destructor's last-resort tail).
#   test_tools          the six tools through the ToolSet and through a
#                       ToolRegistry batch: each tool's happy path, the
#                       ArgumentParse failure for every malformed argument, the
#                       Invoke failure for a session that is gone, the
#                       type/security each tool declares (asserted AFTER
#                       settling, so a write_attributes that never ran cannot
#                       pass), the refusal of an unconfirmed state change, and
#                       one end-to-end turn (spawn -> write -> wait -> read ->
#                       kill).
#
# Both link the toolset, which carries the async event bus transitively through
# tools_intrinsic -> tools_iface: the RequireConfirm tools need a confirmation
# handler, and test_tools subscribes one on a bus IT owns rather than the
# process-wide default_async_bus(), so the tests leave no global subscription
# behind for another suite to inherit.
#
# -pthread on both: the store's coroutines and the registry's parallel branches
# use asio's concurrent primitives (the same reason tools/test's test_registry
# carries the flag), and the per-config option sets are preserved.
function(intrinsic_process_add_test name)
    add_executable(${name} ${name}.cpp)
    target_compile_options(${name} PRIVATE
        -pthread
        $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
    )
    target_link_libraries(${name} PRIVATE
        tools_intrinsic_process
        Boost::unit_test_framework
    )
    add_test(NAME ${name} COMMAND ${name})
    # Real children, real timers: a wedged case must fail rather than hang the
    # whole suite.
    set_tests_properties(${name} PROPERTIES TIMEOUT 180)
endfunction()

intrinsic_process_add_test(test_session_store)
intrinsic_process_add_test(test_tools)
