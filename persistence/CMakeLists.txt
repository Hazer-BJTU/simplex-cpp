# Backend-agnostic authoritative session persistence.
#
# The contract in <persistence/store.hpp> is the only interface the worker uses
# for state saves, restoration, compact archives and archive retention. The JSON
# adapter owns its layout; a database backend (#187) implements the same contract
# without changing the worker. Only this module may know a storage format, so
# dataclass, loop and provider code stay format-independent.
add_library(persistence_lib SHARED
    src/store.cpp src/json_store.cpp src/json_archive.cpp
    src/sqlite_schema.cpp src/sqlite_snapshot.cpp)
target_include_directories(persistence_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(persistence_lib
    PUBLIC dataclass_iface
    PRIVATE load_persistence fileio_lib sqlite_lib)
set_target_properties(persistence_lib PROPERTIES OUTPUT_NAME simplex_persistence)
target_compile_options(persistence_lib PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
