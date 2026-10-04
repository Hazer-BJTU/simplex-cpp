# Qwen

**Last updated: 2026-10-04.** The bundled `qwen` plugin targets the Qianwen AI
platform's OpenAI-compatible Chat Completions endpoint. It supports text and
static images as input and text as output. Audio, video, PDF input, image
generation and platform-managed tools are outside this integration.

## Models and validation

| Model | Text and images | Agent tool calling | Default thinking | Default reasoning replay |
| --- | --- | --- | --- | --- |
| `qwen3.8-flash` | Supported | Supported | Enabled | Disabled |
| `qwen3.8-max` | Supported | Supported | Enabled | Enabled |

These are the plugin's advertised choices. Defaults also apply to dated model
names beginning with either name followed by `-`. Other nonempty startup model
names are accepted for compatible deployments, but their capabilities and
implicit defaults are not inferred. They are not added to the panel's choices.

Coverage is based on offline request/response, plugin-loading and image-tool
tests. No credentialed live verification was performed for this implementation.
Check your account's model access before deployment; availability, image limits
and token limits belong to the provider. See the official
[model catalog guide](https://platform.qianwenai.com/docs/developer-guides/getting-started/model-selection)
and [Flash guide](https://platform.qianwenai.com/docs/developer-guides/getting-started/latest-model).

## Worker configuration

Start with the installed `bin/config.example.yaml`, retaining its client,
persistence and other worker settings. Replace its provider and role selection:

```yaml
providers:
  primary:
    plugin: qwen
    endpoint:
      base_url: https://maas.qianwenaiapi.com
      request_path: /compatible-mode/v1/chat/completions
      auth:
        scheme: bearer
        api_key: ${QWEN_API_KEY}
    model: qwen3.8-flash
    config:
      enable_thinking: true
      reasoning_effort: medium
    retry:
      max_attempts: 3
      initial_backoff_ms: 500
      max_backoff_ms: 120000

driver_model: primary
# Optional independent instance for the local-image assistance tool:
modality_assist_model: primary
```

Export `QWEN_API_KEY` in the environment that starts the worker, including inside
the container when using Docker. This is a user-chosen environment variable name;
the plugin uses ordinary Bearer authentication. The base URL and request path
above are also its defaults. The platform's
[introduction](https://platform.qianwenai.com/docs/developer-guides/getting-started/introduction)
uses the combined SDK base URL ending in `/compatible-mode/v1`; in Simplex,
`request_path` is appended to any base URL prefix. If you use that combined URL,
set `request_path: /chat/completions` to avoid duplicating the prefix.

To retain another driver and use Qwen only for images, keep your existing
`providers.primary` and `driver_model: primary`, add:

```yaml
providers:
  # Keep your existing primary entry here.
  vision:
    plugin: qwen
    endpoint:
      auth:
        scheme: bearer
        api_key: ${QWEN_API_KEY}
    model: qwen3.8-flash
    config:
      enable_thinking: false

modality_assist_model: vision
```

Merge the `vision` entry into the existing `providers` mapping; do not create a
second YAML key with the same name. Omitting `modality_assist_model` leaves image
assistance unloaded. Selecting the same entry for both roles creates separate
model instances; changing driver options does not change the assistant instance.

## Thinking and options

The existing worker options signal advertises:

| Option | Runtime choices |
| --- | --- |
| `model` | `qwen3.8-flash`, `qwen3.8-max` |
| `enable_thinking` | `enabled`, `disabled` |
| `reasoning_effort` | `low`, `medium`, `xhigh` |

Runtime choices are strings, consistent with the worker protocol. The provider
translates `enabled` / `disabled` into Boolean `enable_thinking` on the wire.
Startup `config.enable_thinking` uses YAML `true` / `false` instead. The current
options report effective values, including defaults, without network access.

For Qwen 3.8, omitted effort defaults to `xhigh`. Startup/low-level `high` and
`max` normalize to `xhigh`; the panel advertises only canonical values.
An explicit top-level `reasoning_effort` takes precedence over the shared
`reasoning: { effort: ... }` envelope. Do not copy DeepSeek's `thinking` object.

Advanced startup settings include Boolean `preserve_thinking`, Boolean
`tool_stream`, and integer `thinking_budget` (0–262144 for Qwen 3.8). Effort and
budget cannot both be configured. When only a budget is set, current options
report its band: 0–4096 as `low`, 4097–16384 as `medium`, and larger values as
`xhigh`. An explicit runtime effort selection removes the budget atomically;
other option patches keep it. A rejected patch leaves all previous settings
intact. When thinking is disabled, the effort selection is retained for use
when thinking is enabled again.

Max replays stored assistant reasoning by default; Flash does so only with
`preserve_thinking: true`. Reasoning stays in `reasoning_content`, separate from
visible assistant text. `preserve_thinking: false` suppresses replay without
removing reasoning from local state. Preserve complete provider reasoning when
using Max's default mode. The native fields and their interaction are documented
in the [Chat API reference](https://platform.qianwenai.com/docs/api-reference/chat/openai-chat).
Put these fields directly in `config`: SDK examples using `extra_body` do not
mean an `extra_body` object should be sent in the HTTP request.

The plugin rejects `tool_choice: required`; ordinary automatic tool selection
and supported explicit tool choices use the shared adapter. Consult the
[function-calling guide](https://platform.qianwenai.com/docs/developer-guides/tool-calling/function-calling)
for provider restrictions on tool choices and streaming complex arguments.

## Images and usage

Image content uses `Modality::Image` with an `external_ref` containing an HTTPS
URL or a `data:image/...;base64,...` URL, mapped to the API's `image_url` part.
Multiple image parts keep their order. Unsupported modality/encoding pairs are
rejected rather than sent as text.

The existing `modality_assist` tool reads local files within its combined 16 MiB
input limit and performs one isolated model exchange. All paths must pass local
validation before the request starts; the remote service still validates image
contents and its own size limits. Files must be accessible to the worker (inside
its container when applicable). Images are sent to the configured provider.
Only the tool's textual result and normal metadata enter the driver's history;
its temporary base64 input does not. The auxiliary request follows the existing
non-interruptible tool policy and configured transport retries.

Usage is read through the shared Chat Completions adapter. Cached input tokens
come from `prompt_tokens_details.cached_tokens`; reasoning tokens are already
part of completion usage and are not added twice. Missing usage remains missing.
See the platform's [cache documentation](https://platform.qianwenai.com/docs/developer-guides/run-and-scale/context-cache).

## Hub and verification

Save the worker YAML in the Hub configuration library, choose it with your local
or Docker launch configuration, and create a session. For an existing session,
stop it, apply the saved configuration, then restart. The Hub supplies its
connection endpoints without replacing your Qwen provider configuration.
The model menu receives the options above through the existing options signal.
Selections apply with the next run-starting payload, not during an active run.
They do not rewrite the saved startup configuration or tune the auxiliary model.

For an opt-in live smoke test:

1. Set the API key and start a session using the first configuration example.
2. Send a simple text request and verify the answer and latest token usage.
3. Ask the driver to call `modality_assist` for one local PNG or JPEG, then two
   images together. Check that the descriptions and subsequent driver reply are
   readable and image base64 is absent from the saved driver conversation.
4. Ask for a harmless ordinary tool operation, and verify the tool result and
   follow-up answer. Repeat with Max to exercise reasoning replay.
5. Switch thinking and effort in the model menu, submit another message, and
   verify the reported current choices. Record the model names and test date.

Authentication/permission errors, image rejection and exhausted requests follow
the normal failed-run reporting. A missing plugin indicates an installation or
ABI issue; an unknown remote model may indicate account access or an endpoint
mismatch. Construction and option queries do not contact the service.
`provider_info()` explicitly reports an unsupported operation: this integration
has no verified model-listing API contract for this platform and does not guess a
catalog URL or request DeepSeek's balance endpoint.
