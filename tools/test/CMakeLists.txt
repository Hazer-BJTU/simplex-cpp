# The tool-invocation exception and its bridge to the model_io contract: the
# rendered failure (what()/to_string()), the InvokeReturn it converts into
# (prose in output.raw, failure marker in extras), the marker read back both
# live and after a JSON round trip, and the detached-invocation handler that
# returns the failure as its result. No I/O: nothing here needs a socket.
add_executable(test_invoke_exception test_invoke_exception.cpp)
target_compile_options(test_invoke_exception PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
target_link_libraries(test_invoke_exception PRIVATE
    tools_iface
    Boost::unit_test_framework
)
add_test(NAME test_invoke_exception COMMAND test_invoke_exception)
