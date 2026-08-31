# Contract tests for the process exception types: context retention, the
# what()/to_string() equivalence, and render omission of absent fields.
add_executable(test_exceptions test_exceptions.cpp)
target_link_libraries(test_exceptions PRIVATE
    process_lib
    Boost::unit_test_framework)
add_test(NAME process_exceptions COMMAND test_exceptions)

# Lifecycle tests for ProcessHandle against real but side-effect-free
# children (echo / false / cat / env / sleep — coreutils any POSIX system
# has). Each case drives one handle on a private io_context + strand and
# runs the io to quiescence before asserting, so the tests observe exactly
# the settled states the class comment documents. Richer fixtures with the
# real toolchain belong to the container build later.
add_executable(test_process_handle test_process_handle.cpp)
target_link_libraries(test_process_handle PRIVATE
    process_lib
    Boost::unit_test_framework)
add_test(NAME process_handle COMMAND test_process_handle)

# The heavier lifecycle/abuse suite: 300-round fd/pid leak audit, a
# destruction storm over live children, stdin-after-death handling. The
# binary self-gates on a disposable environment (inside the build container
# via /.dockerenv, or SIMPLEX_DESTRUCTIVE_TESTS=1 for manual/CI runs) and
# passes instantly anywhere else; the LABEL lets a host-side ctest skip the
# whole group with -LE destructive.
add_executable(test_process_destructive
    test_process_destructive.cpp
    scenario.hpp)
target_link_libraries(test_process_destructive PRIVATE
    process_lib
    Boost::unit_test_framework)
add_test(NAME process_destructive COMMAND test_process_destructive)
set_tests_properties(process_destructive PROPERTIES
    LABELS "destructive"
    TIMEOUT 300)
