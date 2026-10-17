# Backend-agnostic authoritative session persistence.
#
# The contract in <persistence/store.hpp> is the only interface the worker uses
# for state saves, restoration, compact archives and archive retention. The JSON
# adapter preserves the historical layout; a database backend (#187) implements
# the same contract without changing the worker. Only this module may know a
# storage format, so dataclass, loop and provider code stay format-independent.
add_library(persistence_lib SHARED src/store.cpp src/json_store.cpp)
target_include_directories(persistence_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# Archive retention still lives in load until #187 PR-B changes the archive
# format; persistence applies it through that compatibility boundary.
target_link_libraries(persistence_lib
    PUBLIC dataclass_iface load_archives
    PRIVATE load_persistence)
set_target_properties(persistence_lib PROPERTIES OUTPUT_NAME simplex_persistence)
target_compile_options(persistence_lib PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>)

if(BUILD_TESTS)
    add_subdirectory(test)
endif()
