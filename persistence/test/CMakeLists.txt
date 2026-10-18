add_executable(test_persistence_store test_store.cpp)
target_link_libraries(test_persistence_store PRIVATE
    persistence_lib load_persistence Boost::unit_test_framework)
add_test(NAME test_persistence_store COMMAND test_persistence_store)

# Archive layout, recognition and retention of the JSON backend: complete-state
# round trips, manifest validation, legacy Markdown preservation.
add_executable(test_persistence_store_archive test_store_archive.cpp)
target_link_libraries(test_persistence_store_archive PRIVATE
    persistence_lib load_persistence Boost::unit_test_framework)
add_test(NAME test_persistence_store_archive COMMAND test_persistence_store_archive)

# SQLite schema v1: creation, version refusal, read-only inspection and
# transactional migrations.
add_executable(test_persistence_sqlite_schema test_persistence_sqlite_schema.cpp)
target_include_directories(test_persistence_sqlite_schema PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test_persistence_sqlite_schema PRIVATE
    persistence_lib sqlite_lib sqlite_runtime Boost::unit_test_framework)
add_test(NAME test_persistence_sqlite_schema
    COMMAND test_persistence_sqlite_schema)

# Normalized row codec: complete-state round trips, presence flags, opaque
# extras, whole-graph rewrites, archive immutability and corruption detection.
add_executable(test_persistence_sqlite_snapshot test_persistence_sqlite_snapshot.cpp)
target_include_directories(test_persistence_sqlite_snapshot PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test_persistence_sqlite_snapshot PRIVATE
    persistence_lib sqlite_lib sqlite_runtime Boost::unit_test_framework)
add_test(NAME test_persistence_sqlite_snapshot
    COMMAND test_persistence_sqlite_snapshot)

# Export the test-only fsync interposer to exercise publication outcomes through
# the store contract, mirroring the load codec's publication test.
add_executable(test_persistence_store_publication test_store_publication.cpp)
target_link_libraries(test_persistence_store_publication PRIVATE
    persistence_lib load_persistence Boost::unit_test_framework)
set_target_properties(test_persistence_store_publication PROPERTIES ENABLE_EXPORTS ON)
add_test(NAME test_persistence_store_publication COMMAND test_persistence_store_publication)

add_executable(test_persistence_sqlite_store test_sqlite_store.cpp)
target_link_libraries(test_persistence_sqlite_store PRIVATE
    persistence_lib sqlite_lib Boost::unit_test_framework)
add_test(NAME test_persistence_sqlite_store COMMAND test_persistence_sqlite_store)

add_executable(test_persistence_sqlite_faults test_sqlite_faults.cpp)
target_link_libraries(test_persistence_sqlite_faults PRIVATE
    persistence_lib sqlite_runtime Boost::unit_test_framework)
add_test(NAME test_persistence_sqlite_faults COMMAND test_persistence_sqlite_faults)

# Complete original-state archives, atomic T1/T2 boundaries and crash retention.
add_executable(test_persistence_sqlite_archive test_sqlite_archive.cpp)
target_include_directories(test_persistence_sqlite_archive PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test_persistence_sqlite_archive PRIVATE persistence_lib sqlite_lib Boost::unit_test_framework)
add_test(NAME test_persistence_sqlite_archive COMMAND test_persistence_sqlite_archive)
