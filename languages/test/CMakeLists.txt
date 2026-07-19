# Language analyzer unit tests.
#
# Each test_*.cpp includes a specific analyzer header (e.g. pythonlang.hpp) and
# exercises that analyzer directly. One executable per test file (Boost.Test
# static linking needs one BOOST_TEST_MODULE/main per binary).
#
# Dependencies:
#   - indextools_iface: brings lang.hpp (the LangAnalyze base class) and the
#     third_party / versioning headers it pulls in. Linked explicitly so a test
#     still compiles even if the analyzer-lib list below is empty.
#   - the analyzer static libs (INDEX_LANG_ANALYZER_LIBS): each exposes its own
#     analyzer header via its PUBLIC include dir and brings indextools_iface
#     transitively (PUBLIC). Linking all of them is harmless — unreferenced
#     objects are dropped — and lets this file stay language-agnostic.
#   - Boost::unit_test_framework: the test harness.
file(GLOB TEST_SOURCES "test_*.cpp")

get_property(_lang_analyzer_libs GLOBAL PROPERTY INDEX_LANG_ANALYZER_LIBS)

foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    set(TEST_EXE "${TEST_NAME}")

    add_executable(${TEST_EXE} ${TEST_SRC})

    target_compile_options(${TEST_EXE} PRIVATE
        $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
    )

    target_link_libraries(${TEST_EXE} PRIVATE
        indextools_iface
        Boost::unit_test_framework
    )
    if(_lang_analyzer_libs)
        target_link_libraries(${TEST_EXE} PRIVATE ${_lang_analyzer_libs})
    endif()

    add_test(NAME ${TEST_EXE} COMMAND ${TEST_EXE})
endforeach()
