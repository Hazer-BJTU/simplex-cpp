add_library(tools_intrinsic_reading SHARED
    src/schemas.cpp src/tools.cpp src/toolset.cpp
    schemas/read_text.yaml schemas/skill.yaml)
target_include_directories(tools_intrinsic_reading PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tools_intrinsic_reading PUBLIC tools_intrinsic)
target_link_libraries(tools_intrinsic_reading PRIVATE textedit_lib)
target_compile_definitions(tools_intrinsic_reading PRIVATE
    SIMPLEX_READING_SCHEMA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/schemas")
target_compile_options(tools_intrinsic_reading PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>)
set_target_properties(tools_intrinsic_reading PROPERTIES OUTPUT_NAME intrinsic_reading_tools)
install(DIRECTORY schemas/ DESTINATION bin/schemas/reading)
if(BUILD_TESTS)
    add_subdirectory(test)
endif()
