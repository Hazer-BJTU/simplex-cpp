add_library(loop_extension_noop MODULE plugin.cpp)
target_link_libraries(loop_extension_noop PRIVATE loop_extensions boost_dll_iface)
set_target_properties(loop_extension_noop PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/loop")
simplex_install_loop_extension_config(
    noop_hook "${CMAKE_CURRENT_SOURCE_DIR}/schemas/config.yaml")
