# protocol — the worker's wire vocabulary, its codec and its published contract.
#
# Two libraries on purpose:
#
#   protocol_lib          descriptors, codec and cross-field rules. It depends on
#                         nlohmann/json only, so a tool or plugin that speaks the
#                         wire format does not acquire a crypto dependency.
#   protocol_manifest_lib the manifest document and the SHA-256 fingerprint. It
#                         uses OpenSSL::Crypto (the project-wide hashing
#                         dependency behind `openssl_iface`) and links the codec.
#
# Worker producers use the codec; only inspection needs manifest generation.
add_library(protocol_lib SHARED src/wire.cpp)
target_include_directories(protocol_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(protocol_lib PUBLIC simplex_thirdparty_iface)
set_target_properties(protocol_lib PROPERTIES OUTPUT_NAME simplex_protocol)
target_compile_options(protocol_lib PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
)

add_library(protocol_manifest_lib SHARED src/manifest.cpp)
target_link_libraries(protocol_manifest_lib PUBLIC protocol_lib PRIVATE openssl_iface)
set_target_properties(protocol_manifest_lib PROPERTIES OUTPUT_NAME simplex_protocol_manifest)
target_compile_options(protocol_manifest_lib PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
)

# The only supported way to produce schemas/worker-protocol.json. It is a normal
# executable built from the same sources, so there is no generation cycle: the
# artifact is the output of running it, never an input to the build.
add_executable(protocol_manifest_export tools/export_manifest.cpp)
target_link_libraries(protocol_manifest_export PRIVATE protocol_manifest_lib)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()

# Reference artifact only: installed inspection always uses compiled descriptors.
install(FILES schemas/worker-protocol.json DESTINATION share/simplex/protocol)
