# Startup configuration

`load` defines the process startup configuration in
[`config.example.yaml`](config.example.yaml). The plugin loading stage is
implemented: it reads YAML, discovers provider modules, and constructs selected
dynamic toolsets and loop hooks. Intrinsic initialization, session registry
construction, model configuration/creation, connection startup, and disk
persistence remain planned work.

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
are not yet consumed or validated. In particular, this stage neither substitutes
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
model, connection, and storage settings describe later implementation stages.

## Template and installation

CMake copies the template unchanged to `<build>/bin/config.example.yaml` and
installs it as `<prefix>/bin/config.example.yaml`. Copy it to a deployment's
`config.yaml`, set its client endpoint, and supply the referenced credentials.
The installer never writes an active `config.yaml`. The example's model and
endpoint values are illustrative, not host defaults or a service availability
guarantee.

```text
<prefix>/bin/
  <host executable>
  config.example.yaml
  config.yaml                  # operator-owned copy
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

There is no version field. The future loader will use the existing
`yamlconfig` YAML-to-JSON boundary and accept a single mapping document.
Unknown host fields are ignored and omitted optional fields use documented
defaults. Missing mappings behave as empty mappings. Explicit null is not a
substitute for a mapping, sequence, or required scalar. Known fields with wrong
types or unusable values report the configuration path and field path.

Compatibility does not mean substituting a different driver model when an
explicit reference cannot be resolved. `driver_model`, its selected provider's
`model`, and `client.endpoint` must be supplied. Plugin-defined generation
options and endpoint `extras` remain opaque to the host; their interpretation
belongs to the model plugin. Existing component YAML parsers retain their own
validation rules.

Environment substitution is limited to provider `endpoint.auth.api_key` and
values in `endpoint.extra_headers`. `${NAME}` substitutes a nonempty environment
variable; `$$` escapes a literal dollar sign. Expansion is one pass, with no
shell evaluation or recursive expansion. An unset or empty referenced variable
is an error when that provider is instantiated. An unused provider does not
require its credentials. Literal credential strings are also accepted; an empty
API key has the existing endpoint meaning of sending no credential header.
Diagnostics must not include expanded credentials or header values.

## Plugin discovery and registration

| Configuration | Omitted default | Meaning |
| --- | --- | --- |
| `plugins.providers.directories` | executable-relative `plugins/llm` | Discover all compatible model-provider modules. |
| `plugins.extensions.tools.directories` | executable-relative `plugins/tools` | Discover dynamic toolset modules. |
| `plugins.extensions.tools.enable` | `[]` | Construct and register the listed toolsets. |
| `plugins.extensions.loop_hooks.directories` | executable-relative `plugins/loop` | Discover dynamic hook modules. |
| `plugins.extensions.loop_hooks.enable` | `[]` | Construct and register hooks in list order. |

Directories are scanned nonrecursively in configured order using the existing
domain loaders and their compatibility checks. Discovery loads native modules;
it is distinct from constructing configured instances. A provider configuration
does not act as a plugin allowlist: all compatible provider descriptors are
loaded even when only one is used by the driver model. Model instances are
constructed when needed.

Every compiled-in intrinsic component is loaded with its existing configuration
mechanism. This public startup contract has no intrinsic enable/disable list,
configuration override, or schema-location override.

Each enabled dynamic entry requires `name`, matching the module's exported
identity. Toolsets may supply `schema_directory`; hooks may supply `config_file`.
Explicit paths take precedence over the existing environment and installed
schema lookups. A missing explicit file does not fall back to another source.
Plugin parameters and tool declarations stay in those component files; the main
configuration does not duplicate or merge them inline. See the
[tool extension guide](../tools/extensions/README.md) and
[hook extension guide](../loop/extensions/README.md).

Repeated names in one enable list, registry name collisions, or failure to
construct an explicitly enabled component are startup errors. The domain
loaders retain their existing handling of rejected modules during discovery;
the host must then verify that the requested components are available. Tools
are registered before hooks. Intrinsic hook order is fixed by the host, followed
by the configured dynamic hook order; directory enumeration never determines
subscription order. Registries remain session-level objects.

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
| `payload_capacity` | `64` | Positive integer; queued incoming payloads. |
| `signal_capacity` | `64` | Positive integer; queued incoming signals. |
| `transport.write_capacity` | `64` | Positive integer; queued outgoing messages. |
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

Only `json` and `if_present` are currently defined values. A future loader must
diagnose an unsupported selected format or restore policy rather than silently
choosing another. Setting `enabled: false` skips all storage operations.
Restoration and automatic saving can be controlled independently through the
save booleans; they do not change loop execution semantics.

Persistence stores the complete `AgentInputState`, including `LoopProgress`,
pending results, and hook state in `extras`. It does not serialize registries,
plugin instances, connections, coroutine frames, queued IO messages, or the
resolved startup configuration. Session identity comes from session state; the
storage filename mapping will be defined with the persistence implementation.

Restoration alone never starts the loop or replays tools. A corrupt or unreadable
existing state is an error, not an absent session. Loop recovery validation must
still decide whether the restored phase can continue. Saving uses a consistent
state at a serialized boundary, after the applicable edit hooks complete; a
failed edit or uncertain tool state must not be presented as a completed step.
Shutdown saving applies only while the host can stop mutation and finish a write,
not after forced termination. Atomic replacement, durability guarantees, and
handling of storage failures will be specified and tested with the disk writer.
