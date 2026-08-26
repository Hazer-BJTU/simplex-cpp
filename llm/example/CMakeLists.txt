# =============================================================================
# example — manual, build-only demonstrations of the llm provider stack (not
# registered with CTest: they need a live provider API key).
#
# llm_deepseek_chat — the provider-stack edition of the DeepSeek chat demo:
# unlike endpoint/example/deepseek_chat.cpp (the hand-rolled interpreter +
# reader + complete prototype), this one drives the shipped machinery —
# LLMDispatcher loads the bundled provider plugins from <exe>/plugins/llm,
# create_model("deepseek") mints the model through the plugin factory, and
# the conversation runs purely through the LLMModel contract (converse +
# integrate), with one parameterised tool (calculate) exercising the full
# schema → wire arguments → parse → execute → correlated-result cycle.
#
# The dependency on llm_deepseek guarantees the provider .so sits under the
# executable's plugins/llm directory, where load_default_models() looks.
# =============================================================================
add_executable(llm_deepseek_chat deepseek_chat.cpp)
target_compile_options(llm_deepseek_chat PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)
target_link_libraries(llm_deepseek_chat PRIVATE llm_iface eventbus_iface)
# No ENABLE_EXPORTS needed: the plugin's converse() publishes on
# eventbus::default_bus(), whose singleton lives in the SHARED eventbus
# library — this executable and the dlopened provider both bind that same
# SONAME, hence the same bus, with no host-side symbol-export convention.
add_dependencies(llm_deepseek_chat llm_deepseek)
