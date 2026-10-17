# Target-side storage management; deliberately independent of the worker runtime.
add_library(manager_lib SHARED src/service.cpp)
target_include_directories(manager_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(manager_lib PUBLIC fileio_lib simplex_thirdparty_iface
    PRIVATE yamlconfig_iface load_persistence)
set_target_properties(manager_lib PROPERTIES OUTPUT_NAME simplex_manager)
add_executable(simplex_manager src/main.cpp)
target_link_libraries(simplex_manager PRIVATE manager_lib Boost::program_options simplex_versioning)
if(BUILD_TESTS)
    add_subdirectory(test)
endif()
