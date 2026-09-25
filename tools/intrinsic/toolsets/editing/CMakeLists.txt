add_library(tools_intrinsic_editing SHARED
    src/schemas.cpp src/tools.cpp src/toolset.cpp
    schemas/str_replace_edit.yaml schemas/skill.yaml)
target_include_directories(tools_intrinsic_editing PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tools_intrinsic_editing PUBLIC tools_intrinsic)
target_link_libraries(tools_intrinsic_editing PRIVATE textedit_lib)
target_compile_definitions(tools_intrinsic_editing PRIVATE
    SIMPLEX_EDITING_SCHEMA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/schemas")
target_compile_options(tools_intrinsic_editing PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>)
set_target_properties(tools_intrinsic_editing PROPERTIES OUTPUT_NAME intrinsic_editing_tools)
install(DIRECTORY schemas/ DESTINATION bin/schemas/editing)
if(BUILD_TESTS)
    add_subdirectory(test)
endif()
