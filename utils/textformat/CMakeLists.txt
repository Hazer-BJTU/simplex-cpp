# Application-independent Markdown framing for metadata and literal output.
add_library(textformat_iface INTERFACE)
target_include_directories(textformat_iface INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
