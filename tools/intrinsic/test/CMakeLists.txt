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
#                   through), a document using the whole argument-schema
#                   vocabulary (the types, description, default, enum, minimum,
#                   minLength, items and anyOf), and every shape it refuses — an
#                   unreadable or unparseable file, a document that is not a
#                   mapping, a missing/empty/mistyped name or description, a
#                   property without a description, a `type` outside the four
#                   kinds this tree's accessors read, a keyword outside the
#                   vocabulary, clauses that disagree with their type or with
#                   each other (a default outside its own enum, a minimum on a
#                   string, an enum member below the minimum), an array without
#                   a string `items`, a `required` naming nobody, and an `anyOf`
#                   branch that introduces a property, restates one, requires
#                   nothing new, or says nothing. The files are written by the
#                   test, which is the only way to reach the shapes a real
#                   declaration should never have.
#   test_skill_declaration
#                   the skill loader: the document one level up from a tool —
#                   what a TOOLSET says about using its tools together. A
#                   well-formed file arriving whole (the guidance is carried
#                   into the system prompt verbatim, so its interior lines are
#                   the part that must not be normalized), a skill stating only
#                   its two required fields, and every shape it refuses — a
#                   document that is not a mapping (the empty file included), a
#                   missing/empty/mistyped name or text, a title or description
#                   present but saying nothing, a keyword list that is not a
#                   list or carries a non-string, an empty or duplicated word,
#                   and a file that cannot be read at all (the path in the
#                   message either way). The reporting form a toolset uses is
#                   pinned too: nullopt instead of a throw.
#
# In-process only: no children, no sockets, and the only I/O is the scratch
# directories test_tool_declaration and test_skill_declaration make for
# themselves. The confirmation cases use a bus the test owns, so nothing is left
# subscribed on the process-wide one.
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
intrinsic_add_test(test_skill_declaration)
