add_library(tools_intrinsic_hub_remote_call SHARED src/tools.cpp)
target_include_directories(tools_intrinsic_hub_remote_call PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tools_intrinsic_hub_remote_call PUBLIC
    tools_intrinsic intercom_exchange)
set_target_properties(tools_intrinsic_hub_remote_call PROPERTIES
    OUTPUT_NAME intrinsic_hub_remote_call_tools)
if(BUILD_TESTS)
    add_subdirectory(test)
endif()
