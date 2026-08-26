# Provider-local tests for the deepseek dialect and model.
#
# test_deepseek_dialect — offline coverage of the dialect policy matrix:
# endpoint defaults, thinking-mode toggle (default enabled, native
# passthrough, none/minimal disable), the high/max effort vocabulary clamp,
# undocumented/deprecated parameter stripping, and the prompt_cache_hit_tokens
# usage bridge. The dialect is header-only, so the test includes it directly.
#
# test_deepseek_model — the full converse() exchange over a loopback HTTP
# server (endpoint/test/loopback_server.hpp): request-body assertions for
# everything the dialect must put on the wire, response assembly through the
# shared chat-completions reader, and the error-chunk exception path.
add_executable(test_deepseek_dialect test_deepseek_dialect.cpp)
target_include_directories(test_deepseek_dialect PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_options(test_deepseek_dialect PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
target_link_libraries(test_deepseek_dialect PRIVATE
    llm_chat_completions Boost::unit_test_framework)
add_test(NAME test_deepseek_dialect COMMAND test_deepseek_dialect)

add_executable(test_deepseek_model test_deepseek_model.cpp)
target_include_directories(test_deepseek_model PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${PROJECT_SOURCE_DIR}/endpoint/test")
target_compile_options(test_deepseek_model PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
target_link_libraries(test_deepseek_model PRIVATE
    llm_chat_completions endpoint_lib eventbus_iface
    Boost::unit_test_framework)
add_test(NAME test_deepseek_model COMMAND test_deepseek_model)
