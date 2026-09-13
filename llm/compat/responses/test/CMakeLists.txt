# Tests for the Responses adapter, next to the code they cover (the layer's own
# CMakeLists adds this directory when BUILD_TESTS is on).
#
# Two of them need endpoint/test on the include path: they drive a whole
# exchange over the endpoint module's loopback transport (loopback_server.hpp),
# which is why they also link endpoint_lib — the adapter itself already does,
# and the fixture is the transport's own test double.
#
# The stream test is the offline counterpart of the streaming path: SSE frames
# in, typed deltas out, no socket involved.

add_executable(test_responses_interpreter test_responses_interpreter.cpp)
target_link_libraries(test_responses_interpreter PRIVATE
    llm_responses Boost::unit_test_framework)
add_test(NAME test_responses_interpreter COMMAND test_responses_interpreter)

add_executable(test_responses_stream test_responses_stream.cpp)
target_link_libraries(test_responses_stream PRIVATE
    llm_responses Boost::unit_test_framework)
add_test(NAME test_responses_stream COMMAND test_responses_stream)

add_executable(test_responses_reader test_responses_reader.cpp)
target_include_directories(test_responses_reader PRIVATE
    "${PROJECT_SOURCE_DIR}/endpoint/test")
target_link_libraries(test_responses_reader PRIVATE
    llm_responses endpoint_lib Boost::unit_test_framework)
add_test(NAME test_responses_reader COMMAND test_responses_reader)

add_executable(test_responses_model test_responses_model.cpp)
target_include_directories(test_responses_model PRIVATE
    "${PROJECT_SOURCE_DIR}/endpoint/test")
target_link_libraries(test_responses_model PRIVATE
    llm_responses endpoint_lib Boost::unit_test_framework)
add_test(NAME test_responses_model COMMAND test_responses_model)
