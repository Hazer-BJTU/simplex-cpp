# =============================================================================
# compat — the LLM compatibility layer
#
# The two adapters that speak an EXTERNAL protocol (openai/chat_completions and
# openai/responses), kept together and apart from the module core: they are the
# only part of llm whose shape is dictated from outside this repository, they
# depend on the transport (endpoint_lib) rather than the other way round, and
# the module contract (include/llm/) must not grow a dependency on either of
# them. See README.md for the boundary, and each subdirectory for its adapter.
#
# Both adapters are SHARED on purpose (the same treatment as utils/asio and
# utils/eventbus): statically linking them into every host executable *and*
# every provider plugin would give each module its own copy of the adapter code,
# its file-level statics, and its weak typeinfo/vtable emissions. Built once as
# libllm_responses.so / libllm_chat_completions.so:
#   - the model and exception classes' typeinfo are STRONG symbols defined in
#     exactly one object per process, so a host catching
#     ChatCompletionsApiException across the dlopen boundary matches by type
#     identity, not by the libstdc++ name-comparison fallback;
#   - the whole async stack (libasio -> libllm_* -> libeventbus) is a single
#     instance per process;
#   - plugins bind the adapter by SONAME (DT_NEEDED) instead of embedding a
#     ~MB static copy, shrinking each provider .so to its thin dialect shell.
# Visibility stays at the GCC default: hosts and plugins both subclass these
# model classes and catch their exception types, so the symbols must export.
# =============================================================================
add_subdirectory(chat_completions)
add_subdirectory(responses)
