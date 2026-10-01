# Architecture

Simplex separates the worker's execution engine from the service that accepts
operator input. Each worker process owns one application instance and session.
The Hub can manage many such workers without sharing their AgentInputState.

```text
Browser / operator
        │ panel HTTP + WebSocket
        ▼
       Hub ───────── configuration library / session metadata / plans
        ▲
        │ persistent event socket + one-shot confirmation / tool sockets
        │ (worker initiates every connection)
        ▼
   core::Application
        ├── load: YAML, plugins, prompts, persistence
        ├── io / intercom: routing, queues, reconnecting transport
        ├── loop: request → model → tools → model → settled outcome
        ├── llm: provider instance → endpoint / protocol adapter
        ├── tools: session ToolRegistry and process/file/remote operations
        └── hooks: session LoopHookRegistry and synchronous loop events
```

## Responsibilities

| Layer | Owns | Does not own |
| --- | --- | --- |
| `core` | Startup, admission, run lifecycle, checkpoints, shutdown | Provider-specific wire parsing |
| `load` | Config parsing, plugin discovery, file persistence helpers | A running agent loop |
| `loop` | Exchange progression, commits, event boundaries, recovery phases | Hub sockets and startup YAML |
| `model_io` dataclasses | Serializable conversation and loop progress | Live sockets, subscriptions, process handles |
| `llm` | Configured provider instances and model exchange interfaces | Tool execution and disk saves |
| `tools` | Dispatch, trust checks, complete tool results | Model inference policy |
| `io` / `intercom` | JSON routing / reusable text WebSocket transport | Conversation semantics |
| Hub | Sessions, launch supervision, operator access, event presentation | Authoritative conversation state |

## Dependency injection and state

The loop receives its model, registry, event bus, state, and options explicitly.
The application assembles those dependencies. Provider and hook instances may
hold process-local state, but restartable conversation state belongs in the
serializable dataclasses. A saved state is not a serialization of the worker
process.

The driver model controls the ordinary conversation. An optional modality
assistant is a separate model instance used by a tool for a single isolated
request; it does not replace the driver's history or silently intercept inputs.

## Startup and ownership

Startup validates configuration, discovers plugin descriptors, constructs the
selected models and extensions, creates session registries, and restores or
initializes state. Persistence ownership prevents cooperating workers from using
the same session root simultaneously. Only after initialization does normal
payload consumption begin.

The application owns the dependency lifetimes through shutdown. Registry handles
keep plugin code alive while instances or tools still need it. Hook subscriptions
are disconnected before their objects are released. Connections and background
process tasks are joined as part of cleanup.

Read [the loop lifecycle](agent-loop.md), [persistence](state-and-persistence.md),
and [security boundaries](security.md) before extending execution behavior.
