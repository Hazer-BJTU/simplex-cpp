add_library(tools_intrinsic_archive_memory SHARED
    src/schemas.cpp src/tools.cpp src/toolset.cpp
    schemas/memory_search.yaml schemas/memory_fetch.yaml schemas/skill.yaml)
target_include_directories(tools_intrinsic_archive_memory PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tools_intrinsic_archive_memory PUBLIC tools_intrinsic persistence_lib
    PRIVATE subprocess_lib)
target_compile_definitions(tools_intrinsic_archive_memory PRIVATE
    SIMPLEX_ARCHIVE_MEMORY_SCHEMA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/schemas")
set_target_properties(tools_intrinsic_archive_memory PROPERTIES OUTPUT_NAME intrinsic_archive_memory_tools)
install(DIRECTORY schemas/ DESTINATION bin/schemas/archive_memory)
if(BUILD_TESTS)
    add_subdirectory(test)
endif()
