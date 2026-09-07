# intercom: internal service-to-service WebSocket communication. Transport
# only — opaque byte strings (JSON in practice) moved over WebSocket — with
# its own request-lifecycle exception (WsException) and retry engine. It
# deliberately does NOT live in endpoint: endpoint's semantic is "application
# requests to model backends" (HTTP/SSE), while intercom is "internal
# communication between services". It does reuse endpoint's connection
# primitives — connect_stream_flavour, the stream flavours, ResolvedEndpoint
# and the retry_policy backoff — rather than re-implementing them.
add_library(intercom_iface INTERFACE)

target_include_directories(intercom_iface INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(intercom_iface INTERFACE
    # endpoint_lib is INTERFACE and already propagates asio_iface,
    # openssl_iface, logging_lib, simplex_thirdparty_iface and
    # dataclass_iface — the connection primitives intercom builds on.
    endpoint_lib
)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
