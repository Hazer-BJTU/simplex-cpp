# dataclass: header-only module defining the model_io data contract. Every
# record is a plain aggregate serialised through nlohmann ADL to_json/from_json
# free functions, so the module depends only on the shared third-party
# interface (for <nlohmann/json.hpp>).
add_library(dataclass_iface INTERFACE)
target_include_directories(dataclass_iface INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(dataclass_iface INTERFACE
    simplex_thirdparty_iface)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
