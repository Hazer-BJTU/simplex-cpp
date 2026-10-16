# One compiled SQLite runtime shared by the host and every project consumer.
# The amalgamation is provisioned locally; configuring never downloads sources.
set(SIMPLEX_SQLITE_SOURCE_DIR "${SIMPLEX_THIRDPARTY_DIR}/include/sqlite" CACHE PATH
    "Directory containing the pinned SQLite sqlite3.c and sqlite3.h")

foreach(_file sqlite3.c sqlite3.h)
    if(NOT EXISTS "${SIMPLEX_SQLITE_SOURCE_DIR}/${_file}")
        message(FATAL_ERROR
            "Missing ${SIMPLEX_SQLITE_SOURCE_DIR}/${_file}. Populate SQLite 3.54.0 "
            "as described in third_party/versions/sqlite.md, or set "
            "SIMPLEX_SQLITE_SOURCE_DIR. Use an updated build image or supply "
            "the source directory explicitly.")
    endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SIMPLEX_SQLITE_SOURCE_DIR}/sqlite3.c"
    "${SIMPLEX_SQLITE_SOURCE_DIR}/sqlite3.h")

# Pin both inputs: a matching header version alone cannot detect modified or
# mismatched implementation sources. See the dependency record for provenance.
file(SHA256 "${SIMPLEX_SQLITE_SOURCE_DIR}/sqlite3.c" _sqlite_source_hash)
file(SHA256 "${SIMPLEX_SQLITE_SOURCE_DIR}/sqlite3.h" _sqlite_header_hash)
if(NOT _sqlite_source_hash STREQUAL
        "2da161e310ef0aba3465f22a4d12b8a43723236f0942fa76e8aac523c3317243"
    OR NOT _sqlite_header_hash STREQUAL
        "cd20a2877bc82b94e36c1093b727dd87cfa3c1d581aac6e240925535cbd90166")
    message(FATAL_ERROR
        "SQLite source/header checksum mismatch; expected the unmodified 3.54.0 amalgamation.")
endif()

find_package(Threads REQUIRED)
add_library(sqlite_runtime SHARED "${SIMPLEX_SQLITE_SOURCE_DIR}/sqlite3.c")
set_target_properties(sqlite_runtime PROPERTIES
    OUTPUT_NAME simplex_sqlite3
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
target_include_directories(sqlite_runtime SYSTEM PUBLIC "${SIMPLEX_SQLITE_SOURCE_DIR}")
# Keep mutex support even when a worker's connections are used serially.
# Loading SQLite extensions is separate from Simplex plugins and is not needed.
target_compile_definitions(sqlite_runtime PRIVATE
    SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION=1)
target_link_libraries(sqlite_runtime PRIVATE Threads::Threads)
if(UNIX)
    target_link_libraries(sqlite_runtime PRIVATE m)
endif()

add_library(sqlite_lib SHARED src/database.cpp)
set_target_properties(sqlite_lib PROPERTIES OUTPUT_NAME simplex_sqlite)
target_include_directories(sqlite_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(sqlite_lib PRIVATE sqlite_runtime)
target_compile_options(sqlite_lib PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
