# Modality assistance

`ModalityAssistToolSet` lets the driver call `modality_assist` to obtain a text
interpretation of local images together from a separate conversation model. Core adds
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
  "path": ["/root/workspace/before.png", "/root/workspace/after.png"],
  "request": "Compare the two screenshots and describe what changed.",
  "extra_modality": "vision"
}
```

`path` is a required nonempty array of nonempty strings; a single image also
uses an array. Order and duplicates are preserved. `request` is a required
nonempty string. `extra_modality` defaults to
`vision`, currently its only supported value. `system_prompt` optionally replaces
the default instruction to describe accurately, answer in text, state uncertainty,
and avoid tool calls. It applies only to this auxiliary request. The image-text
instruction in that default is guidance, not a fixed security boundary: callers
that replace the system prompt must include their own guidance.

The tool identifies PNG, JPEG, GIF, and WebP from the file header, including
images without an extension. For example, a PNG named `diagram.jpg` is sent as
`image/png`. A conflicting or unrecognized suffix produces a hint with the
zero-based path index. An extensionless image with a recognized header needs no hint.
If the header is unrecognized, the tool falls back to `.png`, `.jpg`, `.jpeg`,
`.gif`, or `.webp` (case-insensitive) and reports the inference. If neither
identifies a supported format, the whole call fails before model IO.
Header recognition does not validate the complete file. The configured
provider decodes the original bytes. NUL paths, missing or
non-regular files, empty files, and a combined input size over 16 MiB fail before
model invocation. The byte limit covers all original file bytes in the list,
including repeated entries. All images are read and encoded before the request
is submitted. Any failure aborts the entire call, including a failure after
previous images were prepared; none are silently omitted. File errors identify
the zero-based `path` index. Image parsing failures reported by the provider also
fail the whole call, without a follow-up attempt using only some images.
Relative paths use the process working directory. Symlinks are followed; there
is no workspace sandbox. Reads check the opened descriptor, cannot block waiting
for a FIFO writer, and do not promise a snapshot of concurrent external edits.

The tool declares `ParallWrite` / `Trusted`: the provider request is an externally
observable effect, while independent calls may run concurrently.
It makes an external model request: all chosen files are transmitted to the
configured auxiliary endpoint without a separate confirmation prompt.

## Request and result

Each invocation constructs a fresh `AgentInputState` containing the selected
system prompt and one user turn with request text followed by images in list
order. Each image is
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
`[[file_bytes]]`, optional `[[hints]]` and `[[token_cost]]`, followed by a `description` block.
Each `media_type` is the MIME type used in the corresponding image data URL.
Errors also include format hints collected before failure. `file_bytes` is the
combined original size of all submitted images.
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
It checks defaults, exact image bytes and ordering, both provider wire formats,
total size limits, all-or-nothing preparation, isolated requests, file and model failures, model ownership, and cancellation draining.
Core tests cover conditional tool/skill registration and normal driver runs.
