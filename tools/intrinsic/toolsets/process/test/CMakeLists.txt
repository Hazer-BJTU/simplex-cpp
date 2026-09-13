# Toolset-local tests for the process family. Three translation units, all
# driving real (but harmless) children — echo / cat / sleep / true / seq / sh,
# the same coreutils process/'s own suite uses:
#
#   test_session_store  the session table on its own: id minting (monotonic,
#                       never reused), the delta/full read distinction and its
#                       per-stream cursors, the difference between a finished
#                       child and a finished capture, the deadline override,
#                       the session cap, waiting with and without a deadline,
#                       the refusal to release a live child, and both shutdown
#                       paths (terminate_all and the destructor's last-resort
#                       tail, on one worker thread and on three).
#   test_tools          the six tools through the ToolSet and through a
#                       ToolRegistry batch: each tool's happy path, the
#                       ArgumentParse failure for every malformed argument, the
#                       Invoke failure for a session that is gone, the
#                       type/security each tool declares for a given settled
#                       call (asserted AFTER settling, so a write_attributes
#                       that never ran cannot pass), the defaults materialized
#                       into the settled query, the refusal of an unconfirmed
#                       state change, and one end-to-end turn
#                       (spawn -> write -> wait -> read -> kill).
#   test_registry_e2e   the composition the agent loop actually uses:
#                       ToolRegistry + ProcessToolSet + ProcessSessionStore +
#                       its own bus + real children, on a context with SEVERAL
#                       worker threads, with every call going through
#                       ToolRegistry::execute — mixed batches (serial first,
#                       then the parallel ones), the settled query and the
#                       confirmation question asserted at the registry
#                       boundary, the scheduling rules measured with a
#                       recording probe tool, and a repeated turn that gives an
#                       ordering regression somewhere to show up.
#
# All three link the toolset, which carries the async event bus transitively
# through tools_intrinsic -> tools_iface: the RequireConfirm tools need a
# confirmation handler, and both tool suites subscribe one on a bus THEY own
# rather than the process-wide default_async_bus(), so the tests leave no global
# subscription behind for another suite to inherit.
#
# -pthread on all three: the store's coroutines and the registry's parallel
# branches use asio's concurrent primitives (the same reason tools/test's
# test_registry carries the flag), and the per-config option sets are preserved.
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
intrinsic_process_add_test(test_registry_e2e)
