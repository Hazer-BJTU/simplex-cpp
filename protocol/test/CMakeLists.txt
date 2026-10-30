add_executable(test_protocol_codec test_codec.cpp)
target_compile_definitions(test_protocol_codec PRIVATE
    PROTOCOL_BOUNDARY_CASES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/boundary-cases.json")
target_link_libraries(test_protocol_codec PRIVATE
    protocol_lib Boost::unit_test_framework)
target_compile_options(test_protocol_codec PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
)
add_test(NAME test_protocol_codec COMMAND test_protocol_codec)

# The staleness check needs the committed artifact, and the check has to be able
# to name it: a failure is only actionable if it says which file to regenerate.
add_executable(test_protocol_manifest test_manifest.cpp)
target_compile_definitions(test_protocol_manifest PRIVATE
    PROTOCOL_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../schemas/worker-protocol.json")
target_link_libraries(test_protocol_manifest PRIVATE
    protocol_manifest_lib Boost::unit_test_framework)
target_compile_options(test_protocol_manifest PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
)
add_test(NAME test_protocol_manifest COMMAND test_protocol_manifest)
