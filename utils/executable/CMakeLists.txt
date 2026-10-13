# Executable discovery is shared by provider loading and subprocess helpers,
# without making the model interface depend on the process implementation.
add_library(executable_iface INTERFACE)
target_include_directories(executable_iface INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(executable_iface INTERFACE boost_dll_iface ${CMAKE_DL_LIBS})

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
