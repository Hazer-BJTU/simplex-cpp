add_executable(test_http_request_exception test_http_request_exception.cpp)

target_link_libraries(test_http_request_exception PRIVATE
    exceptions_lib
    Boost::unit_test_framework
)

add_test(NAME test_http_request_exception COMMAND test_http_request_exception)
