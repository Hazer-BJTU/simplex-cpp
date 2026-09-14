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
#   test_tool_declaration
#                   the YAML declaration loader: a well-formed file arriving
#                   whole and VERBATIM (the argument schema is what a provider
#                   puts on the wire, so nothing may be normalized on the way
#                   through), and every shape it refuses — an unreadable or
#                   unparseable file, a document that is not a mapping, a
#                   missing/empty/mistyped name or description, a schema that is
#                   not an object schema, a property that is not a mapping, and
#                   a `required` that names a property nobody declared. The
#                   files are written by the test, which is the only way to
#                   reach the shapes a real declaration should never have.
#
# In-process only: no children, no sockets, and the only I/O is the scratch
# directory test_tool_declaration makes for itself. The confirmation cases use a
# bus the test owns, so nothing is left subscribed on the process-wide one.
function(intrinsic_add_test name)
    add_executable(${name} ${name}.cpp)
    target_compile_options(${name} PRIVATE
        $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
    )
    target_link_libraries(${name} PRIVATE
        tools_intrinsic
        Boost::unit_test_framework
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

intrinsic_add_test(test_tool_base)
intrinsic_add_test(test_tool_declaration)
