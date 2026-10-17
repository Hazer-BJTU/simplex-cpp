add_executable(test_persistence_store test_store.cpp)
target_link_libraries(test_persistence_store PRIVATE
    persistence_lib load_persistence Boost::unit_test_framework)
add_test(NAME test_persistence_store COMMAND test_persistence_store)

# Export the test-only fsync interposer to exercise publication outcomes through
# the store contract, mirroring the load codec's publication test.
add_executable(test_persistence_store_publication test_store_publication.cpp)
target_link_libraries(test_persistence_store_publication PRIVATE
    persistence_lib load_persistence Boost::unit_test_framework)
set_target_properties(test_persistence_store_publication PROPERTIES ENABLE_EXPORTS ON)
add_test(NAME test_persistence_store_publication COMMAND test_persistence_store_publication)
