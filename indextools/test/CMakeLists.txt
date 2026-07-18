# Collect all test source files
file(GLOB TEST_SOURCES "test_*.cpp")

# Build one executable per test file (Boost.Test static linking requires
# one BOOST_TEST_MODULE/main per binary)
foreach(TEST_SRC ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
    set(TEST_EXE "${TEST_NAME}")

    # Most tests exercise the editor/utils layer; a couple have bespoke needs:
    #   - test_subprocess drives the async SubProcessManager.
    #   - test_plugin_manager loads the real language plugins at runtime, so it
    #     compiles the plugin manager and links Boost.DLL instead of the
    #     static analyzer library.
    if(TEST_NAME STREQUAL "test_subprocess")
        set(TEST_EXTRA_SOURCES "${INDEXTOOLS_SOURCE_DIR}/src/subprocess.cpp")
    elseif(TEST_NAME STREQUAL "test_plugin_manager")
        set(TEST_EXTRA_SOURCES "${INDEXTOOLS_SOURCE_DIR}/src/plugin_manager.cpp")
    elseif(TEST_NAME STREQUAL "test_cache_system")
        # SearchInterface drives the real plugin loader (like test_plugin_manager)
        # and additionally needs the cache engine and glob-based utils.
        set(TEST_EXTRA_SOURCES
            "${INDEXTOOLS_SOURCE_DIR}/src/plugin_manager.cpp"
            "${INDEXTOOLS_SOURCE_DIR}/src/cache_system.cpp"
            "${INDEXTOOLS_SOURCE_DIR}/src/utils.cpp")
    elseif(TEST_NAME STREQUAL "test_viewer")
        # The read interface is pure: it only needs viewer.cpp (split.hpp and
        # schema.hpp are header-only).
        set(TEST_EXTRA_SOURCES "${INDEXTOOLS_SOURCE_DIR}/src/viewer.cpp")
    elseif(TEST_NAME STREQUAL "test_logger")
        # Logger is self-contained: only needs its own .cpp.
        set(TEST_EXTRA_SOURCES "${INDEXTOOLS_SOURCE_DIR}/src/logger.cpp")
    elseif(TEST_NAME STREQUAL "test_shell")
        # SpawnWaitCommand::execute is a coroutine defined in the header that
        # references SubProcessManager::spawn_and_wait — the linker needs the
        # symbol even though ensure_params-only tests never call it.
        set(TEST_EXTRA_SOURCES "${INDEXTOOLS_SOURCE_DIR}/src/subprocess.cpp")
    else()
        set(TEST_EXTRA_SOURCES
            "${INDEXTOOLS_SOURCE_DIR}/src/utils.cpp"
            "${INDEXTOOLS_SOURCE_DIR}/src/editor.cpp")
    endif()

    add_executable(${TEST_EXE} ${TEST_SRC} ${TEST_EXTRA_SOURCES})

    target_include_directories(${TEST_EXE} PRIVATE
        "${INDEXTOOLS_INCLUDE_DIR}"
        "${SIMPLEX_THIRDPARTY_DIR}/include"
    )

    target_compile_options(${TEST_EXE} PRIVATE
        $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
    )

    # Boost::process is needed by subprocess.cpp (Boost.Process v2 + Asio).
    target_link_libraries(${TEST_EXE} PRIVATE
        Boost::unit_test_framework
        Boost::process
    )

    # Language targets are discovered by languages/ and exposed via GLOBAL
    # properties, so tests never name an individual language.
    get_property(_lang_plugins      GLOBAL PROPERTY INDEX_LANG_PLUGINS)
    get_property(_lang_analyzer_libs GLOBAL PROPERTY INDEX_LANG_ANALYZER_LIBS)

    if(TEST_NAME STREQUAL "test_plugin_manager" OR TEST_NAME STREQUAL "test_cache_system")
        # Runtime plugin loader: needs Boost.DLL and every plugin .so on disk.
        target_link_libraries(${TEST_EXE} PRIVATE boost_dll_iface)
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

    # Opt-in ThreadSanitizer build of the cache test. It instruments the
    # multithreaded_* cases, guarding the multi-threaded io_context path
    # against data races in the cache machinery (strand-protected map access,
    # payload sharing, etc.). The analyzer sources compiled into the target
    # (cache_system.cpp etc.) are instrumented alongside; the runtime-loaded
    # language plugins stay uninstrumented, which is fine — the cache and
    # channel are only touched from instrumented code.
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
