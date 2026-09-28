add_library(tools_intrinsic_modality_assist SHARED
    src/schemas.cpp src/tools.cpp src/toolset.cpp
    schemas/modality_assist.yaml schemas/skill.yaml)
target_include_directories(tools_intrinsic_modality_assist PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tools_intrinsic_modality_assist PUBLIC tools_intrinsic llm_iface)
target_link_libraries(tools_intrinsic_modality_assist PRIVATE fileio_lib)
target_compile_definitions(tools_intrinsic_modality_assist PRIVATE
    SIMPLEX_MODALITY_ASSIST_SCHEMA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/schemas")
target_compile_options(tools_intrinsic_modality_assist PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>)
set_target_properties(tools_intrinsic_modality_assist PROPERTIES
    OUTPUT_NAME intrinsic_modality_assist_tools)
install(DIRECTORY schemas/ DESTINATION bin/schemas/modality_assist)
if(BUILD_TESTS)
    add_subdirectory(test)
endif()
