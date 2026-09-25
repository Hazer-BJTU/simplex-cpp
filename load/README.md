# Startup configuration

`load` defines the process startup configuration in
[`config.example.yaml`](schemas/config.example.yaml). The plugin loading stage is
implemented: it reads YAML, discovers provider modules, and constructs selected
dynamic toolsets and loop hooks. Explicit JSON snapshot IO and Markdown session
export are also implemented. The core worker now consumes the full startup
configuration, initializes intrinsics and registries, constructs the driver model,
starts IO and applies persistence policy. Plugin-only entry points remain usable
independently. See [core](../core/README.md) for the runtime contract.

| Surface | Current implementation |
| --- | --- |
| `plugins` | Parse YAML, discover all provider descriptors, construct selected extensions. |
| Intrinsic components and registries | Initialized by core, not plugin-only loading. |
| `providers`, `driver_model` | Selected provider validated/expanded by read_configuration; constructed by core. |
| `client` | Parsed by read_configuration; started by core. |
| `persistence` | Parsed by read_configuration; applied by core. |
| `security.confirmation`, `worker` | Approval endpoint/deadline, event capacity, exchange budget and initial prompt. |
| `save_state()`, `load_state()` | Explicit file operations, independent of startup YAML. |

The intended host runs one agent loop at a time. Startup configuration selects
deployment resources; conversation and recovery state remain in
`model_io::AgentInputState`. Configuration changes take effect on the next
startup. Hot reload is outside this contract.

## Implemented loading functions

Link `load_lib` and include [`load/plugins.hpp`](include/load/plugins.hpp):

```cpp
auto plugins = load::load_plugins("/etc/simplex/config.yaml");

// Provider descriptors are ready for later create_model() calls.
auto& providers = plugins.providers;

// The host owns session registries and decides when registration takes place.
for (auto& toolset : plugins.extensions.tools) {
    tool_registry.add(std::move(toolset));
}
for (auto& hook : plugins.extensions.loop_hooks) {
    hook_registry.add(std::move(hook));
}
```

`load_plugins(file)` validates all plugin configuration sections before opening
native modules. `load_providers(configuration, configuration_directory)` and
`load_extensions(configuration, configuration_directory)` also accept a complete
JSON document that a host has already parsed. Their base directory must be
absolute, and each validates only its own section. Unknown fields are ignored.
The unrelated `providers`, `driver_model`, `client`, and `persistence` sections
are not consumed or validated by these plugin-only functions. In particular, this stage neither substitutes
environment variables nor needs model credentials or a running IO executor.

All functions are synchronous startup operations. They return fresh ownership
bundles and do not modify caller-owned registries or subscribe hooks. Product
construction follows enable-list order; unselected products do not read YAML or
invoke product factories. An empty enable list skips discovery for that domain.
For nonempty lists, discovery still opens candidate native modules and invokes
descriptor factories to learn their exported names. Selection is not a promise
that unselected library initializers cannot run. Native libraries remain subject
to the existing process-resident loading policy.

Malformed known configuration fields and unavailable/failed selected products
throw `PluginLoadError`, with the relevant field path. The file entry point adds
the source filename and wraps YAML or discovery errors in the same exception.
The JSON extension entry point propagates filesystem enumeration errors. Domain
loaders retain their existing missing-directory and module rejection policies;
selected names must still resolve to successfully constructed instances.
Failures release already-created products without returning a partial bundle;
native initialization side effects cannot be undone. Returned products retain
their modules even after the temporary loaders are destroyed. Configure and
register everything at a serialized startup boundary before beginning loop work.

`test_load_plugins` covers the shipped template, provider discovery across
multiple directories, relative paths, default paths, selective construction and
ordering, instance lifetime through registry use, configuration/factory failures,
and compatible parsing. All of these tests run without network access or API
credentials. The remaining sections specify the complete startup contract;
model, connection, and automatic storage settings are consumed by the core worker. The explicit persistence API below is usable independently of startup
policy loading.

## Session snapshots and readable exports

Link `load_lib` and include
[`load/persistence.hpp`](include/load/persistence.hpp). Both save modes take an
explicit filename and a const reference to the session:

```cpp
load::save_state("./sessions/current.json", state);
auto restored = load::load_state("./sessions/current.json");

load::ReadableOptions preview;
preview.max_json_string_bytes = 1024;
preview.max_json_items = 32;
preview.max_json_depth = 6;
preview.max_json_block_bytes = 8192;
load::save_state("./memory/current.md", state, load::StateFormat::Readable, preview);
```

`StateFormat::Json` writes the existing `AgentInputState` JSON representation,
including structured prompt sections, all conversation content, tool arguments
and results, loop progress, pending results, and extras. It never clips data or
updates session IDs/timestamps. The host owns those values. One JSON tree is
built for serialization; the `AgentInputState` itself is not copied. Non-finite
numbers and nlohmann binary/discarded values are rejected because JSON text
cannot preserve them. `ContentType::Binary` remains supported through its normal
base64 string representation.

`load_state()` accepts JSON only and returns a new state, with no mutation of an
existing session. It requires the four top-level dataclass fields `meta`,
`system_prompt`, `tools`, and `turns`, checks typed record containers, and then
uses the existing dataclass decoder. Unknown fields, omitted optional fields,
and legacy single-object message content retain their compatibility behavior.
A missing, unreadable, truncated, or malformed file throws `PersistenceError`
with its path; it never becomes a fresh empty session implicitly. Loading does
not decide whether the loop may resume, execute tools, or replay pending results.
That remains the host's recovery responsibility.

`StateFormat::Readable` is a write-only Markdown export for people and model
memory lookup. It contains session metadata, loop progress and pending tool
results, system prompt sections, tool definitions, and chronological turns and
agent steps. Message roles, reasoning, call IDs, tool results, token costs,
retention preferences, commit sequences, and extras remain distinguishable.
Repeated tool output stored both as message content and as its result record is
shown once when identical. Binary content is summarized by encoded size;
external references remain visible without fetching them.

Ordinary prose is retained in full. The total Markdown export size is not
bounded. Each JSON preview separately limits string and key bytes, entries per
container, nesting depth, and total rendered bytes. Complete JSON objects/arrays
encoded inside text content (common for tool results) receive the same
treatment. Such text is parsed one content part at a time; parsing still needs
memory proportional to that part. JSON fields already stored as JSON are
traversed by reference, and rendering stops at the block budget. The exporter
does not first serialize the whole session.

Truncation is marked explicitly, with omission counts where practical. Clipped
previews may no longer be valid JSON; they are for reading, not parsing or state
restoration. Limits never drop a later turn or an entire later Markdown section,
and UTF-8 prefixes are not cut inside a code point. Message text and previews use
fences longer than embedded backtick runs so their content cannot introduce
false document headings. The source state remains unchanged. This export does
not perform redaction or automatically index the document into a memory store.

File publication delegates to [`fileio::atomic_write`](../utils/fileio/README.md),
including descriptor ownership, temporary-file cleanup, and buffered output.
`load` owns only snapshot serialization and readable rendering.

These are synchronous operations. The caller must serialize saving with state
mutation and coordinate writers to the same destination. Snapshot writes create
missing parent directories and use an exclusively opened mode-0600 temporary
file in the destination directory. The file is flushed, synced, and closed
before atomic rename, followed by a parent-directory sync. A failure before
rename leaves the old destination intact and cleans up the temporary file. If
directory sync fails after rename, `PersistenceError::published()` returns true:
the new file is visible, but crash durability is uncertain. All pre-publication
save failures and all load failures report false. A destination symlink is
replaced, not followed. Newly created ancestor directories are not individually
synced, so this is not an unconditional power-loss durability guarantee for a
newly created directory tree. Forced termination before publication can leave a
temporary file; automatic cleanup of such abandoned files is not implemented.

JSON and Markdown destinations are independent operations, with no transaction
across the two. Neither API interprets startup YAML, derives filenames from
session IDs, subscribes to loop events, or performs asynchronous/background IO.
The template's `persistence.format: json` continues to describe restorable
snapshots; Markdown export is selected explicitly through this API.

## Template and installation

CMake copies `load/schemas/config.example.yaml` unchanged to
`<build>/bin/config.example.yaml` and installs it as
`<prefix>/bin/config.example.yaml`. Copy it to a deployment's
`config.yaml`, set its client endpoint, and supply the referenced credentials.
The installer never writes an active `config.yaml`. The example's model and
endpoint values are illustrative, not host defaults or a service availability
guarantee.

```text
<prefix>/bin/
  <host executable>
  config.example.yaml
  config.yaml                  # operator-owned copy
  prompts/
    coding_agent.yaml         # default prompt for new sessions
  plugins/
    llm/
    tools/
    loop/
  schemas/                     # existing component declarations/configuration
  data/sessions/               # example persistence location
```

Explicit relative paths resolve against the main configuration file's parent
directory, regardless of the process working directory. Absolute paths remain
absolute. Empty or omitted plugin directory lists use the existing defaults
relative to the executable, not the configuration file. A nonempty list replaces
the corresponding default. There is no implicit shell expansion of paths.

## Parsing and compatibility

There is no version field. The implemented plugin loader uses the existing
`yamlconfig` YAML-to-JSON boundary and accepts a single mapping document.
Plugin-only entry points validate `plugins`. `read_configuration()` validates
the selected provider and the host IO, security, worker and persistence settings.
Unknown host fields are ignored and omitted optional fields use documented
defaults. Missing mappings behave as empty mappings. Explicit null is not a
substitute for a mapping, sequence, or required scalar. Known fields with wrong
types or unusable values report the configuration path and field path.

For model and client startup, compatibility does not mean
substituting a different driver model when an explicit reference cannot be
resolved. `driver_model`, its selected provider's `model`, and `client.endpoint`
must be supplied. Plugin-defined generation options and endpoint `extras` remain
opaque to the host; their interpretation belongs to the model plugin. Existing
component YAML parsers retain their own validation rules.

Environment substitution in `read_configuration()` is limited to provider `endpoint.auth.api_key` and values in
`endpoint.extra_headers`. `${NAME}` substitutes a nonempty environment variable;
`$$` escapes a literal dollar sign. Expansion is one pass, with no shell
evaluation or recursive expansion. An unset or empty referenced variable is an
error when that provider is instantiated. An unused provider does not require
its credentials. Literal credential strings are also accepted; an empty API key
has the existing endpoint meaning of sending no credential header. Diagnostics
must not include expanded credentials or header values.

## Plugin discovery and registration

| Configuration | Omitted default | Meaning |
| --- | --- | --- |
| `plugins.providers.directories` | executable-relative `plugins/llm` | Discover all compatible model-provider modules. |
| `plugins.extensions.tools.directories` | executable-relative `plugins/tools` | Discover dynamic toolset modules. |
| `plugins.extensions.tools.enable` | `[]` | Construct the listed toolsets; registration is caller-owned. |
| `plugins.extensions.loop_hooks.directories` | executable-relative `plugins/loop` | Discover dynamic hook modules. |
| `plugins.extensions.loop_hooks.enable` | `[]` | Construct hooks in list order; registration is caller-owned. |

Directories are scanned nonrecursively in configured order using the existing
domain loaders and their compatibility checks. Discovery loads native modules;
it is distinct from constructing configured instances. A provider configuration
does not act as a plugin allowlist: all compatible provider descriptors are
loaded even when only one is used by the driver model. Core constructs the selected model after parsing host configuration.

Core loads every compiled-in intrinsic component with its existing configuration
mechanism. Plugin-only loading does not initialize intrinsics. This public startup contract has no intrinsic enable/disable list,
configuration override, or schema-location override.

Each enabled dynamic entry requires `name`, matching the module's exported
identity. Toolsets may supply `schema_directory`; hooks may supply `config_file`.
Explicit paths take precedence over the existing environment and installed
schema lookups. A missing explicit file does not fall back to another source.
Plugin parameters and tool declarations stay in those component files; the main
configuration does not duplicate or merge them inline. See the
[tool extension guide](../tools/extensions/README.md) and
[hook extension guide](../loop/extensions/README.md).

Repeated names in one enable list or failure to construct an explicitly enabled
component are loader errors. Registry collision detection belongs to the host.
The domain loaders retain their existing handling of rejected modules during
discovery; the host must then verify that the requested components are
available. Core registers tools before hooks. Intrinsic
hook order is fixed by the host, followed by the configured dynamic hook order;
directory enumeration never determines subscription order. Registries remain
session-level objects.

## Providers and the driver model

`providers` maps configuration names to endpoint/model definitions. Each name
is a host reference, not necessarily a plugin name. For example, two entries
named `direct` and `proxy` can both set `plugin: deepseek`, with different base
URLs, credentials, models, or generation parameters. `driver_model: proxy`
selects the latter for the agent loop. No other model role is defined yet.

| Provider field | Omitted behavior | Meaning |
| --- | --- | --- |
| `plugin` | Use the provider configuration's key. | Descriptor/factory name. |
| `endpoint` | Use the selected plugin's endpoint defaults. | Deployment configuration. |
| `model` | Required when instantiated. | Nonempty provider model name. |
| `config` | `{}` | Provider-specific generation options. |
| `retry` | Use the model plugin's retry defaults. | Request retry policy. |

`endpoint` follows [`ModelEndpoint`](../dataclass/include/dataclass/endpoint_config.hpp):

- `base_url` carries scheme, host, optional port, and optional path prefix.
  `request_path` is appended to that prefix. Missing endpoint fields inherit
  the selected plugin's defaults, including provider-specific paths.
- `auth.scheme` accepts `none`, `bearer`, or `custom_header`. A nonempty API key
  produces the corresponding credential header; `header_name` applies only to
  `custom_header` and defaults to `x-api-key` in the shared endpoint record.
- `user_agent` supplies the HTTP user agent. `extra_headers` maps header names
  to strings applied after standard headers, including authentication headers.
- `extras` is optional provider endpoint metadata. An empty mapping is not
  required; omission preserves the existing optional-field semantics.

The loader assembles the model factory's existing configuration shape by
copying `config` into the root object, then supplying `model`, the selected
plugin name as `provider`, and any supplied `endpoint` and `retry` objects.
Omitted endpoint fields stay omitted so the provider can apply its own defaults.
The keys `model`, `provider`, `endpoint`, and `retry` are reserved and must not
also appear inside `config`; conflicting sources are errors. Remaining
generation keys pass through without a host-maintained whitelist.

The bundled adapters accept `retry.max_attempts`, `initial_backoff_ms`, and
`max_backoff_ms`. Despite its existing name, `max_attempts` counts retries
**after** the initial attempt: `3` permits up to four attempts, and `0` disables
retries. Delays are positive integer milliseconds, with the maximum at least
the initial delay. The template uses the current bundled defaults of three
retries, 500 ms initial delay, and 120000 ms maximum delay. Retry eligibility
remains the model transport's recoverability policy; it is not the WebSocket
reconnection policy.

## Client connection

`client.endpoint` is a complete `ws://` or `wss://` URL with a nonempty host,
optional port, and the upgrade target including any query string. Omitted
ports follow the scheme, and an empty path means `/`. TLS follows the scheme
and uses the existing verified TLS context. Custom WebSocket authentication
headers and TLS overrides are not exposed by this template.

| Field | Default | Constraint / behavior |
| --- | --- | --- |
| `payload_capacity` | `256` | Positive integer; queued incoming payloads. |
| `signal_capacity` | `256` | Positive integer; queued incoming signals. |
| `transport.write_capacity` | `256` | Positive integer; queued outgoing messages. |
| `transport.initial_backoff_ms` | `250` | Positive integer milliseconds. |
| `transport.max_backoff_ms` | `10000` | Integer milliseconds, at least the initial delay. |
| `transport.idle_timeout_seconds` | `0` | Nonnegative integer seconds; zero disables idle timeout. |

These options map to `io::ClientOptions` and
`intercom::StableWebSocketOptions`. Capacities count messages, not bytes.
Positive idle timeout enables the existing idle ping and peer-response deadline.
All connection-establishment failures are retried indefinitely with capped
backoff, including DNS, TLS verification, and upgrade rejection. Protocol errors
and signal-handler failures are fatal. This policy is fixed, not a configurable
retry classification in this template.

The existing `type`/`data` envelope routes payloads and signals. Payload overflow
rejects and counts a message; signal overflow ends the client. The default
signal handler publishes synchronously on the injected EventBus. There is one
payload subscriber with one outstanding `next()` operation. Outgoing admission
does not guarantee delivery, and a failed write is not automatically replayed.
Queues survive reconnection but are not disk-persisted. The client has a
single-use `run()`; shutdown closes admission and requires awaiting `run()`
before releasing its dependencies. See [IO](../io/README.md) and
[intercom](../intercom/README.md) for the full lifecycle contract.

## Disk persistence policy

| Field | Default | Meaning |
| --- | --- | --- |
| `enabled` | `true` | Enable restoration and saving of session state. |
| `directory` | `./data/sessions` | Storage root, relative to the configuration file. |
| `format` | `json` | Existing `AgentInputState` dataclass serialization. |
| `restore` | `if_present` | Restore an existing session; create fresh state only when absent. |
| `save.on_step_finished` | `true` | Save after a completed step's state-edit hooks. |
| `save.on_run_finished` | `true` | Save after run completion and its state-edit hooks. |
| `save.on_shutdown` | `true` | Save during orderly shutdown after mutation stops. |

The format is `json`; restoration accepts `if_present` or `never`. Unsupported
values are startup errors. `never` starts fresh and may replace an existing file. Setting `enabled: false` skips all storage operations.
Restoration and automatic saving can be controlled independently through the
save booleans; they do not change loop execution semantics.

Persistence stores the complete `AgentInputState`, including `LoopProgress`,
pending results, and hook state in `extras`. It does not serialize registries,
plugin instances, connections, coroutine frames, queued IO messages, or the
resolved startup configuration. Session identity comes from session state; the
worker stores JSON at <directory>/<session-id>/state.json.

Restoration alone never starts the loop or replays tools. A corrupt or unreadable
existing state is an error, not an absent session. Loop recovery validation must
still decide whether the restored phase can continue. Saving uses a consistent
state at a serialized boundary, after the applicable edit hooks complete; a
failed edit or uncertain tool state must not be presented as a completed step.
Shutdown saving applies only while the host can stop mutation and finish a write,
not after forced termination. The explicit writer's atomic replacement and
durability boundaries are described above. Core schedules the stable checkpoints and stops new admission after a required
JSON failure. See its recovery contract for details.

## Full worker configuration

Include load/configuration.hpp and call read_configuration(file) to validate
host settings before native loading. parse_configuration(document, directory)
supports an already parsed document and requires an absolute base directory.
Both return Configuration, a runtime settings bundle rather than a persisted
session dataclass. Model credentials are expanded only for driver_model.

The optional security.confirmation mapping selects a complete ws:// or wss://
endpoint and a positive timeout_ms (default 120000). Missing configuration
means confirmation-required calls are denied. The endpoint is independent from
client.endpoint; the timeout invalidates approval across the whole exchange.
It is not a hard bound on return latency: an already-running system DNS backend
may delay completion and shutdown until it returns. Late results remain denied.

worker.max_exchanges defaults to 512 and event_capacity to 1024.
worker.system_prompt_file selects a structured YAML prompt for new sessions.
persistence.readable defaults to false and enables an additional Markdown
export. JSON remains authoritative. See core for safety checkpoints, cancellation
saving and failure policy; the explicit persistence APIs do not interpret YAML.


## System prompt files

The default file is maintained in `core/prompts/coding_agent.yaml` and exported
as `<build>/bin/prompts/coding_agent.yaml` and
`<prefix>/bin/prompts/coding_agent.yaml`. The startup template selects it using:

```yaml
worker:
  system_prompt_file: ./prompts/coding_agent.yaml
```

An explicit path is resolved relative to the main configuration file, regardless
of the working directory. Absolute paths are accepted. If the field is omitted,
the loader reads `<executable_dir>/prompts/coding_agent.yaml`. Empty paths and
missing files fail startup; there is no embedded text fallback. If an operator
copies the example configuration to another directory, copy the prompt beside it
or change this path. The old inline `worker.system_prompt` field is rejected with
a migration error. Plugin-only loading APIs do not read prompt files.

```yaml
heading_level: 2
sections:
  - name: persona
    title: ""
    stability: immutable
    text: |
      You are a helpful assistant.
      Follow the available tool guidance.
  - name: notes
    title: Session notes
    stability: growing
    text: ""
  - name: context
    title: Current context
    stability: volatile
    text: ""
```

The root must be a mapping with a `sections` list; an empty list is permitted.
`heading_level` defaults to 2 and must be an integer in 1..6. Each section requires
a unique, nonempty string `name` and string `text`. `title` defaults to an empty
string (no heading), and `stability` defaults to `immutable`. Sections must be
ordered by stability: `immutable`, then `growing`, then `volatile`. Duplicate
names, unknown stability values, wrong field types, and names beginning with
`skill.` or equal to `environment.runtime` are rejected. These names are reserved
for host-injected skills and runtime environment hints.
Unknown additional fields are tolerated. The existing PromptTemplate text
normalization and Markdown rendering rules apply; prompt text does not undergo
environment-variable substitution.

`load::read_system_prompt(path)` performs synchronous YAML loading and validation,
returning an owned `model_io::PromptTemplate`. `read_configuration` and
`parse_configuration` invoke it during startup and place the result in
`Configuration::system_prompt`. Invalid or unreadable files fail with filename
context. Hosts constructing Configuration directly must supply their own parsed
prompt; a default-constructed Configuration has an empty prompt and performs no IO.

The worker moves this prompt into a **new** AgentInputState and then injects
current tool skills. For a restored session, the snapshot's complete prompt is
retained and only host-owned skill sections are rebuilt. Editing the YAML file
therefore affects newly created sessions, not restored ones. The file is still
validated on every startup, including restoration; removing it can prevent
startup even when a snapshot exists. There is no hot reload or remote prompt
replacement operation.

## Runtime environment hints

Optional `worker.environment` settings describe the worker to the model:

```yaml
worker:
  environment:
    workspace: ./project
    platform: Linux x86_64
    software:
      - Python 3.12
      - Docker CLI
```

`workspace` and `platform` are strings; `software` is a list of strings. Missing
or empty values are omitted. Unknown fields are tolerated; malformed known
fields are rejected. Relative workspace paths resolve against the configuration
file's directory without requiring the path to exist. These settings do not
change the process working directory, restrict tool access, or detect/verify
platform and software availability. They are operator-supplied prompt hints.

At startup, core replaces the host-owned `environment.runtime` section with
current configuration, including when restoring a session. It is Volatile and
appears after tool skills, before user-defined Volatile sections. Empty settings
remove any old section without adding a new one. The section is saved with the
session, is not hot-reloaded, and remains editable through existing loop hooks.
