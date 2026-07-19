# logging unit tests.
#
# One executable per test_*.cpp (Boost.Test static linking needs one
# BOOST_TEST_MODULE/main per binary). Each links logging_lib — its PUBLIC
# include dir brings logging/logger.hpp, and the static objects provide
# Logger's out-of-line definitions.
file(GLOB TEST_SOURCES "test_*.cpp")

foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    set(TEST_EXE "${TEST_NAME}")

    add_executable(${TEST_EXE} ${TEST_SRC})

    target_compile_options(${TEST_EXE} PRIVATE
        $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
    )

    target_link_libraries(${TEST_EXE} PRIVATE
        logging_lib
        Boost::unit_test_framework
    )

    add_test(NAME ${TEST_EXE} COMMAND ${TEST_EXE})
endforeach()
