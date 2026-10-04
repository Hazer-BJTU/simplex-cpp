# Develop a model provider

A provider translates shared model dataclasses into a remote model protocol,
performs one complete exchange, and integrates its response. It does not execute
tools, own the agent loop, or write conversation snapshots to disk.

## Choose an adapter

Reuse `llm::chat_completions::ChatCompletionsModel` or
`llm::responses::ResponsesModel` when the endpoint speaks one of those protocols.
A dialect supplies endpoint defaults and protocol-specific behavior. DeepSeek and Qwen
illustrate Chat Completions dialects; the in-tree OpenAI implementation
illustrates the Responses adapter.

Unsupported modality/encoding combinations must fail explicitly. Do not silently
reinterpret document, image, or audio input as a different category. Preserve
provider metadata required for valid conversation replay.

## Exported interface

```cpp
std::unique_ptr<extension::ExtensionContext> create_llm_plugin();
std::unique_ptr<llm::LLMModel> create_llm_model(
    boost::asio::any_io_executor executor,
    const nlohmann::json& config);
```

The descriptor derives from `llm::LLMModelExtensionContext`, reports
`llm::LLM_PLUGIN_ABI_VERSION`, and names the provider factory. Export aliases
with these exact signatures plus `SIMPLEX_EXPORT_PLUGIN_MAGIC`.
The model constructor receives its executor and complete configuration. The
factory reference is temporary; the instance must own what it retains.

The dispatcher performs the model build lifecycle before exposing the configured
instance. Follow [llm/models.hpp](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/llm/include/llm/models.hpp)
for lifecycle and ownership contracts. Model exchange coroutines must propagate
cancellation safely; they must not leave references into destroyed request state.

## Build and configure

```cmake
add_library(llm_example MODULE plugin.cpp)
target_link_libraries(llm_example PRIVATE llm_chat_completions boost_dll_iface)
set_target_properties(llm_example PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins/llm")
```

Use `llm_responses` instead if using that adapter. Add the provider subdirectory
to the build, install it with the worker, then select it in YAML:

```yaml
providers:
  primary:
    plugin: example
    endpoint:
      base_url: https://model.example
      request_path: /v1/chat/completions
      auth:
        scheme: bearer
        api_key: ${MODEL_API_KEY}
    model: example-model
    config: {}
driver_model: primary
```

Endpoint defaults may be overridden by YAML. Retry settings are provider request
policy, separate from loop exchange limits. Validate and translate provider
options instead of forwarding unrelated host configuration into API requests.

## Runtime options

Override the const `get_options()` to return local descriptors:

```json
[{"name":"model","options":["example-model"]}]
```

`get_current_options()` returns effective selections. `handle_options()` validates
the entire patch before changing settings. Unknown keys and invalid values must
not leave partially applied options. The host calls this synchronously before a
new invocation; the options signal only reads choices and current values.

Do not make option discovery depend on a live API request. Test failures from
request construction, transport, provider rejection, exhausted retries, and
response parsing. Usage reporting should preserve prompt, generated, and cache-hit
meaning so hooks and clients can calculate costs consistently.
