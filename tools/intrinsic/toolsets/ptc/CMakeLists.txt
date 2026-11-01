add_library(tools_intrinsic_ptc SHARED src/runtime.cpp src/toolset.cpp src/schemas.cpp)
target_include_directories(tools_intrinsic_ptc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tools_intrinsic_ptc PUBLIC tools_intrinsic_process PRIVATE subprocess_lib)
target_compile_definitions(tools_intrinsic_ptc PRIVATE
    SIMPLEX_PTC_SCHEMA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/schemas")
install(DIRECTORY schemas/ DESTINATION bin/schemas/ptc)
if(BUILD_TESTS)
    add_subdirectory(test)
endif()
