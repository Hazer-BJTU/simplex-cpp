# Text-file inspection and editing, independent of model-facing tools.
add_library(textedit_lib SHARED src/utf8_probe.cpp)
target_include_directories(textedit_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(textedit_lib PRIVATE fileio_lib)
set_target_properties(textedit_lib PROPERTIES OUTPUT_NAME simplex_textedit)
target_compile_options(textedit_lib PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
