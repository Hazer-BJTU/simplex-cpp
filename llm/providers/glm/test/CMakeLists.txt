# Provider-local tests for the glm dialect and model.
#
# test_glm_dialect — offline coverage of the dialect policy matrix: endpoint
# defaults, the always-on thinking policy of the GLM-5.3 family, the
# medium/xhigh aliases against the same family, the refusal of none/minimal,
# request hygiene (n, the ignored penalties, tool_choice, tool_stream), and the
# finish_reason mapping. The dialect is header-only, so the test includes it
# directly.
#
# test_glm_model — the full converse() exchange over a loopback HTTP server
# (endpoint/test/loopback_server.hpp): request-body assertions for everything
# the dialect must put on the wire, response assembly through the shared
# chat-completions reader, the content-filter verdict, the error-chunk
# exception path, and the build gate.
add_executable(test_glm_dialect test_glm_dialect.cpp)
target_include_directories(test_glm_dialect PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_compile_options(test_glm_dialect PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
)
target_link_libraries(test_glm_dialect PRIVATE
    llm_chat_completions Boost::unit_test_framework)
add_test(NAME test_glm_dialect COMMAND test_glm_dialect)

add_executable(test_glm_model test_glm_model.cpp)
target_include_directories(test_glm_model PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${PROJECT_SOURCE_DIR}/endpoint/test")
target_compile_options(test_glm_model PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
)
target_link_libraries(test_glm_model PRIVATE
    llm_chat_completions endpoint_lib eventbus_iface
    Boost::unit_test_framework)
add_test(NAME test_glm_model COMMAND test_glm_model)
target_compile_definitions(test_glm_model PRIVATE
    GLM_PLUGIN_DIR="$<TARGET_FILE_DIR:llm_glm>")
set_target_properties(test_glm_model PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(test_glm_model llm_glm)
