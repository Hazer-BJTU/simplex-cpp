add_library(tools_intrinsic_hub_remote_call SHARED src/tools.cpp src/plan.cpp src/schemas.cpp)
target_include_directories(tools_intrinsic_hub_remote_call PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tools_intrinsic_hub_remote_call PUBLIC
    tools_intrinsic intercom_exchange)
set_target_properties(tools_intrinsic_hub_remote_call PROPERTIES
    OUTPUT_NAME intrinsic_hub_remote_call_tools)
if(BUILD_TESTS)
    add_subdirectory(test)
endif()

target_compile_definitions(tools_intrinsic_hub_remote_call PRIVATE
    SIMPLEX_HUB_REMOTE_CALL_SCHEMA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/schemas")
install(DIRECTORY schemas/ DESTINATION bin/schemas/hub_remote_call)
