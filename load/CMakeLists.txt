# Ship editable startup configuration without embedding it in a binary.
# COPYONLY preserves environment references such as ${DEEPSEEK_API_KEY}.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/config.example.yaml"
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/config.example.yaml"
    COPYONLY)

# Install only the example, never an operator's active config.yaml.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/config.example.yaml"
    DESTINATION bin)
