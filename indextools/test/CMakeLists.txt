# Collect all test source files
file(GLOB TEST_SOURCES "test_*.cpp")

# Build one executable per test file (Boost.Test static linking requires
# one BOOST_TEST_MODULE/main per binary). Each test links indextools_lib, which
# transitively brings in the project headers, third_party headers, and the
# Boost.Process / Boost.DLL link deps — so a test no longer has to hand-pick
# src/*.cpp files or redeclare include paths.
foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    set(TEST_EXE "${TEST_NAME}")

    add_executable(${TEST_EXE} ${TEST_SRC})

    target_compile_options(${TEST_EXE} PRIVATE
        $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
    )

    target_link_libraries(${TEST_EXE} PRIVATE
        indextools_lib
        Boost::unit_test_framework
    )

    # Language targets are discovered by languages/ and exposed via GLOBAL
    # properties, so tests never name an individual language.
    get_property(_lang_plugins      GLOBAL PROPERTY INDEX_LANG_PLUGINS)
    get_property(_lang_analyzer_libs GLOBAL PROPERTY INDEX_LANG_ANALYZER_LIBS)

    if(TEST_NAME STREQUAL "test_plugin_manager" OR TEST_NAME STREQUAL "test_cache_system")
        # Runtime plugin loader: needs every plugin .so on disk at the path
        # compiled into the lib (LANG_PLUGIN_DIR is defined on these tests).
        # indextools_lib already links Boost::dll transitively.
        target_compile_definitions(${TEST_EXE} PRIVATE
            "LANG_PLUGIN_DIR=\"${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins\"")
        if(_lang_plugins)
            add_dependencies(${TEST_EXE} ${_lang_plugins})
        endif()
    elseif(_lang_analyzer_libs)
        # Header-including tests (e.g. test_pythonlang) link the static analyzer
        # libs; each lib's PUBLIC include dir brings in its analyzer header.
        # Linking all of them is harmless — unreferenced objects are dropped.
        target_link_libraries(${TEST_EXE} PRIVATE ${_lang_analyzer_libs})
    endif()

    add_test(NAME ${TEST_EXE} COMMAND ${TEST_EXE})

    # Opt-in ThreadSanitizer build of the cache test. The cache machinery
    # (cache_system.cpp etc.) now lives in indextools_lib, so when ENABLE_TSAN
    # is on the parent CMakeLists instruments the lib's sources too; here we
    # only add the link-time runtime and the test's own compile flags. The
    # runtime-loaded language plugins stay uninstrumented, which is fine — the
    # cache and channel are only touched from instrumented code.
    #
    # Caveat: this will NOT reliably flag a revert from concurrent_channel back
    # to plain channel. basic_channel uses a null_mutex internally (so the race
    # is real), but launch_search's single-consumer/rendezvous handshake routes
    # every send/receive completion through the io_context scheduler's own
    # mutex, which gives TSan a happens-before edge and masks the channel race.
    # The result_channel fix is justified by Boost's thread-safety contract
    # (basic_channel: shared objects Unsafe) and the null_mutex implementation,
    # not by TSan reproduction.
    if(ENABLE_TSAN AND TEST_NAME STREQUAL "test_cache_system")
        target_compile_options(${TEST_EXE} PRIVATE
            -fsanitize=thread -g -O1 -fno-omit-frame-pointer)
        target_link_options(${TEST_EXE} PRIVATE -fsanitize=thread)
        # halt_on_error makes the first race fail the test outright.
        set_tests_properties(${TEST_EXE} PROPERTIES
            ENVIRONMENT "TSAN_OPTIONS=halt_on_error=1 second_deadlock_stack=1")
    endif()
endforeach()
