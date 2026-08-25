# Unit tests for the llm model-plugin contract (llm/models.hpp).
#
# test_llm_models — in-memory coverage only: the model-type vocabulary, the
# default converse() throw, the integrate() placement table (incl. orphan
# folds and the persistence guarantee), and the dispatcher path over in-memory
# contexts (add + create_model routing, the build/release lifecycle guarantee
# incl. build failure, the ABI/type/warm admission gates). The real dynamic
# path (a toy provider .so under bin/plugins/llm, load_models over it) is
# deferred together with the first concrete provider and the full-flow tests.
#
# llm_iface already carries Boost headers (Asio), nlohmann/json,
# extension_framework, and the generated versioning header.

add_executable(test_llm_models test_models.cpp)
target_compile_options(test_llm_models PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
target_link_libraries(test_llm_models PRIVATE llm_iface Boost::unit_test_framework)
add_test(NAME test_llm_models COMMAND test_llm_models)
