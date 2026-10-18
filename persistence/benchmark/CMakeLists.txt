# Explicit developer target only. SimplexRelease excludes benchmark directories
# from installation independently of whether this target was built manually.
add_executable(benchmark_persistence store.cpp)
target_link_libraries(benchmark_persistence PRIVATE persistence_lib)
