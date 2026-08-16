# endpoint: the header-only transport layer (endpoint_lib) plus the compiled
# Responses-API compatibility layer (responses_api_lib). Consumers link
# endpoint_lib to inherit the public include path together with Boost and
# OpenSSL dependencies; concrete request interpreters and their SSE handlers
# are compiled libraries of their own by design — they stay out of the
# header-only interface surface.
add_library(endpoint_lib INTERFACE)

target_include_directories(endpoint_lib INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(endpoint_lib INTERFACE
    Boost::headers
    openssl_iface
    logging_lib
    simplex_thirdparty_iface
    # The model request interpreter interface consumes the data contract
    # (AgentInputState) and the provider endpoint configuration records.
    dataclass_iface
)

# ========================================== #
# Build responses_api_lib (static library)   #
# ========================================== #
#
# The OpenAI Responses-API compatibility layer: a concrete
# ModelRequestInterpreter (AgentInputState -> POST /responses body), a
# dedicated SSEResponseHandler decoding the event stream into ResponsesDelta
# blocks (decode only), and a ModelResponseReader consuming those deltas —
# accumulation, assembly into the model_io contract record, hooks and the
# handler lifecycle. Asio/Beast are used header-only, so no compiled Boost
# components are needed here.
add_library(responses_api_lib STATIC
    src/responses/interpreter.cpp
    src/responses/stream_handler.cpp
    src/responses/reader.cpp
)
target_include_directories(responses_api_lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_link_libraries(responses_api_lib PUBLIC
    endpoint_lib
)
target_compile_options(responses_api_lib PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
)
if(LTO_SUPPORT)
    set_target_properties(responses_api_lib PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
        INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE
    )
endif()

# Keep tests optional under the project-wide BUILD_TESTS switch.
if(BUILD_TESTS)
    add_subdirectory(test)
endif()

# Interactive examples against live HTTPS services. Built unconditionally (a
# single small translation unit); not registered with CTest.
add_subdirectory(example)
