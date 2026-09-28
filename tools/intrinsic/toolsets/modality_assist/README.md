# Modality assistance

`ModalityAssistToolSet` lets the driver call `modality_assist` to obtain a text
interpretation of one local image from a separate conversation model. Core adds
this intrinsic set and its skill only when `modality_assist_model` is configured
and successfully constructed. Omitting that model omits the set. This is a
dependency-injected intrinsic: it has no provider discovery or default model.

Link `tools_intrinsic_modality_assist`, include
`tools/intrinsic/modality_assist/toolset.hpp`, and construct the set with an
already-built `std::shared_ptr<llm::LLMModel>`. Null and non-conversation models
are rejected. Shared ownership keeps the model alive; the tool neither builds
nor releases it. Concurrent calls use independent temporary states and rely on
the model interface's reentrant `converse` contract.

## Invocation

```json
{
  "path": "/root/workspace/screenshot.png",
  "request": "Describe the layout and transcribe the error message.",
  "extra_modality": "vision"
}
```

`path` and `request` are required nonempty strings. `extra_modality` defaults to
`vision`, currently its only supported value. `system_prompt` optionally replaces
the default instruction to describe accurately, answer in text, state uncertainty,
and avoid tool calls. It applies only to this auxiliary request.

PNG, JPEG (`.jpg` or `.jpeg`), GIF, and WebP suffixes are accepted without regard
to case. The suffix supplies the MIME type and is only a plausibility check;
the tool does not decode, resize, transcode, or verify image contents. Actual
format support belongs to the configured provider. NUL paths, missing or
non-regular files, empty files, and files over 16 MiB fail before model invocation.
Relative paths use the process working directory. Symlinks are followed; there
is no workspace sandbox. Reads check the opened descriptor, cannot block waiting
for a FIFO writer, and do not promise a snapshot of concurrent external edits.

The tool declares `ReadOnly` / `Trusted`, consistent with local reading tools.
It makes an external model request: the chosen file is transmitted to the
configured auxiliary endpoint without a separate confirmation prompt.

## Request and result

Each invocation constructs a fresh `AgentInputState` containing the selected
system prompt and one user turn with request text plus an image. The image is
`ExternalRef` / `Image`, with a `data:image/...;base64,...` URL. This preserves
the image through both Chat Completions and Responses adapters without changing
their content contracts. The state has no tools or historical turns and is moved
into exactly one `converse` call. The provider's configured transport retries
still apply; the tool does not retry failed inference or run a follow-up loop.

Once invoked, the tool awaits completion with coroutine cancellation disabled.
Cancelling the driver therefore waits at the existing tool-batch boundary until
the provider completes or fails. There is no extra timeout beyond the provider's
normal request policy.

Success uses the shared intrinsic text format: `[[path]]`, `[[media_type]]`,
`[[file_bytes]]`, optional `[[token_cost]]`, followed by a `description` block.
Text content parts are joined with newlines. Reasoning and internal response
metadata are omitted. Empty/non-text replies and any attempted tool calls fail;
no auxiliary tools are executed. File and provider errors become normal tool
errors, allowing the driver to decide what to do next.

Only normal tool arguments/results enter driver history. Raw image payloads and
the auxiliary conversation are not attached to the driver's `AgentInputState`.
The auxiliary model's description remains generated source material rather than
an authoritative observation or an instruction to the driver.

## Declarations and tests

Runtime YAML comes from nonempty `SIMPLEX_MODALITY_ASSIST_SCHEMA_DIR`, then
`<executable>/schemas/modality_assist`, then the compiled source directory.
An explicit override is authoritative. Installation includes both the shared
library and YAML files. Missing/malformed declarations follow the normal
intrinsic behavior: an unusable tool is not advertised, and a missing skill
does not remove an otherwise usable tool.

`test_modality_assist_tools` uses an asynchronous fake model and real registry.
It checks defaults, exact image bytes, both provider wire formats, isolated
requests, file and model failures, model ownership, and cancellation draining.
Core tests cover conditional tool/skill registration and normal driver runs.
