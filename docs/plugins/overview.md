# Plugin principles

Simplex has three extension domains: tools, loop hooks, and model providers.
They use native shared modules, domain descriptors, exported factories, and
shared C++ interfaces. Built-in tools/hooks use the same runtime interfaces but
do not need a dynamically loaded descriptor.

## Discovery and construction

```text
startup configuration
    → scan library directories
    → load library / validate build fingerprint and descriptor
    → resolve typed factory
    → read selected configuration
    → construct instance
    → register for this worker session
```

Discovery and construction are separate. All compatible provider descriptors
are discovered; model instances are created only for selected roles. Tools and
hooks are constructed only when named in their `enable` lists. Empty lists do
not disable built-in components.

| Domain | Descriptor | Runtime product | Owner |
| --- | --- | --- | --- |
| Tools | `ToolSetExtensionContext` | `ToolSet` with `ToolInterface` handles | `ToolRegistry` |
| Loop | `LoopHookExtensionContext` | `LoopHookInterface` | `LoopHookRegistry` |
| Models | `LLMModelExtensionContext` | `LLMModel` | Application model role |

## Lifetime

Factory configuration references are valid only during construction. Copy needed
values; do not store references into temporary YAML/JSON objects. Returned
instances must be fully initialized or construction must fail with RAII cleanup.

The generic loader uses process-resident modules on the supported Linux path
(`RTLD_NODELETE`); dropping handles is not a hot-unload mechanism.
Library ownership is also retained through instance destruction. Retained tool handles
must keep the originating module alive too. Hook bindings disconnect before
releasing their object; the event bus must outlive bindings. Do not unload or
replace an instance while its callback or coroutine is executing.

Persistent state belongs in serializable AgentInputState fields. In-memory caches,
subscriptions, process handles, and connections are rebuilt after restart.

## Compatibility and trust

The framework checks a build fingerprint, the domain context, ABI version,
factory exports, and descriptor identity. Modules use C++ types across library
boundaries, so build them with the same compiler/runtime/dependency context as
the host. A matching ABI number alone is insufficient.

These checks are not a sandbox. Library initialization may execute before
validation. Configure only trusted plugin directories; see
[security](../architecture/security.md).
