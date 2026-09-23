add_library(tools_extension_noop MODULE plugin.cpp)
target_link_libraries(tools_extension_noop PRIVATE tools_extensions boost_dll_iface)
set_target_properties(tools_extension_noop PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/tools")
simplex_install_tool_extension_schema(
    noop_toolset "${CMAKE_CURRENT_SOURCE_DIR}/schemas")
