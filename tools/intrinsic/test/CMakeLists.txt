# Tests for the intrinsic shared core, with no toolset involved — these are the
# pieces a second toolset family inherits unchanged, so a regression in them
# would otherwise surface as a puzzling failure in whichever family noticed
# first.
#
#   test_tool_base  IntrinsicTool's argument accessors (null-as-absent, the
#                   refuse-rather-than-coerce rule for every type, the signed/
#                   unsigned integer trap, element-wise list checking and the
#                   messages a model reads), the JSON result shape, the schema
#                   builders, and the routing of a confirmation at the tool's
#                   own bus (approved, refused by silence, Trusted unasked).
#                   Also IntrinsicToolSet: presentation order vs. routing, a
#                   tool that will not build, duplicate and unnamed tools, and
#                   one call through the phases the base deliberately inherits
#                   rather than overrides.
#
# In-process only: no children, no sockets. The confirmation cases use a bus the
# test owns, so nothing is left subscribed on the process-wide one.
add_executable(test_tool_base test_tool_base.cpp)
target_compile_options(test_tool_base PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
target_link_libraries(test_tool_base PRIVATE
    tools_intrinsic
    Boost::unit_test_framework
)
add_test(NAME test_tool_base COMMAND test_tool_base)
