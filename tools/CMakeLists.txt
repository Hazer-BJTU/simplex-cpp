# =============================================================================
# tools — module (core package)
#
# The tool-invocation layer: the model_io tool contract (Invocable /
# InvokeQuery / InvokeReturn) turned into something that runs. `tools_iface`
# is header-only and carries three headers:
#
#   - invoke_exception.hpp  the tool-side failure type and its bridge to the
#                           InvokeReturn record a detached invocation reports
#                           back on;
#   - security_check.hpp    the default invocation security policy, whose
#                           RequireConfirm case asks the host through an
#                           InvokeConfirmEvent published on the async bus;
#   - toolsets.hpp          ToolInterface / ToolSet: one tool, and the set
#                           that dispatches a call and runs it through every
#                           checkpoint.
#
# It links `async_eventbus_iface` (utils/async_eventbus) for the confirmation
# event: the policy publishes on eventbus::default_async_bus(), and that
# singleton is defined in the SHARED `async_eventbus_lib` precisely so a host
# in one module and a toolset in another — a dlopened plugin included — bind
# ONE bus per process by SONAME. An inline singleton would fork a private bus
# per module and the confirmation would never reach the UI that can answer it.
# =============================================================================
add_library(tools_iface INTERFACE)
target_include_directories(tools_iface INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tools_iface INTERFACE
    extensions_iface
    dataclass_iface
    simplex_versioning
    asio_iface
    async_eventbus_iface
)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
