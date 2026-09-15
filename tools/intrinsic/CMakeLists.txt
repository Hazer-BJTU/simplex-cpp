# =============================================================================
# tools/intrinsic — the host's own tools (core package)
#
# The tools a host offers without loading anything: built into the tree and
# registered by the host itself, as opposed to the plugin-provided tools the
# extension framework brings. Organised the way llm/ organises its providers —
# a shared core here, one self-contained package per toolset under toolsets/ —
# with one deliberate difference: an intrinsic toolset is NOT a plugin. It is a
# shared library a host links directly, so there is no ExtensionContext, no
# signed factory and no dlopen; a host constructs the set and hands it to its
# ToolRegistry.
#
# THIS TARGET (tools_intrinsic / libintrinsic_tools.so) is the shared core every
# toolset derives from — the counterpart of llm_iface + the protocol adapters
# for a provider:
#
#   tool_base.hpp     IntrinsicTool: the Invocable's storage, the argument
#                     accessors (which refuse rather than coerce), the JSON
#                     result shape, the JSON-Schema builders, and the routing of
#                     a RequireConfirm confirmation at a chosen event bus.
#                     DeclaredTool is the IntrinsicTool whose Invocable comes
#                     from a declaration file instead.
#   tool_declaration.hpp  one tool's name, description and argument schema as a
#                     YAML document, with the loader that reads and validates it
#                     (load_tool_declaration) and the reporting form a tool uses
#                     so that a broken file costs one tool rather than the host
#                     (try_load_tool_declaration).
#   skill_declaration.hpp  the same idea one level up: a TOOLSET's skill — the
#                     prose about how its tools are used together — as a YAML
#                     document, with load_skill_declaration() and the reporting
#                     form a set uses, so a broken file costs the guidance
#                     rather than the capability.
#   toolset_base.hpp  IntrinsicToolSet: the ordered catalogue get_tools() hands
#                     out, the name->tool table dispatch() routes by, the
#                     tools' build()/release() lifecycle, and the set's skill
#                     (load_skill() / skill()). It deliberately does NOT
#                     override prepare()/execute() — those carry the invocation
#                     layer's checkpoint sequence and failure contracts, and an
#                     in-process set wants them unchanged.
#
# A toolset package therefore supplies only its domain: its tools, their
# declarations, its skill, and whatever state they share.
#
# YAML DECLARATIONS, and why the YAML reader is a PRIVATE dependency here: a
# declaration file is a document (comments, block scalars, anchors), and
# yamlconfig/yaml_json.hpp is the project's one YAML boundary. It is linked
# PRIVATE because it is an implementation detail of the loader — no public
# header of this library mentions yamlconfig — and because yamlconfig is itself
# a SHARED library (utils/yamlconfig), this target depends on libyamlconfig.so
# rather than compiling yaml-cpp into itself. That is what keeps a process with
# several such consumers down to one copy of the YAML reader.
#
# SHARED, following docs/abi-context.md: these are classes a host and a dlopened
# plugin may both subclass or catch across the boundary, so their typeinfo,
# vtables and inline emissions must resolve to ONE authoritative copy per
# process. Statically linking would give each module its own copy and break
# catch-by-type-identity — the same reasoning that makes the protocol adapters
# and the async runtime shared.
# =============================================================================
add_library(tools_intrinsic SHARED
    src/skill_declaration.cpp
    src/tool_base.cpp
    src/tool_declaration.cpp
    src/toolset_base.cpp
)
target_include_directories(tools_intrinsic PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tools_intrinsic PUBLIC
    tools_iface
    dataclass_iface
    asio_iface
    logging_lib
)
# The YAML reader behind the declaration loader; PRIVATE on purpose, see above.
target_link_libraries(tools_intrinsic PRIVATE yamlconfig_iface)
target_compile_options(tools_intrinsic PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
set_target_properties(tools_intrinsic PROPERTIES OUTPUT_NAME intrinsic_tools)
if(LTO_SUPPORT)
    set_target_properties(tools_intrinsic PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()

# ---- toolsets ---------------------------------------------------------------
# One package per family, each with its own include/src/test and its own
# library, linking the core above. Added after it so a package's
# target_link_libraries names a target that already exists.
add_subdirectory(toolsets/process)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
