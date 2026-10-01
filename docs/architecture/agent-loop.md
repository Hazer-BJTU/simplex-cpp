# Agent-loop execution

An invocation starts with a new user message or a request to continue existing
state. The host serializes invocations even when an `io_context` uses several
execution threads. Payloads wait behind active work; control signals have an
independent consumer.

## Normal exchange

```text
admit input → integrate user message → await model request
                                      │
                              commit model response
                                      │
                         tool calls? ─┴─ no → finish
                              │ yes
                        checkpoint tools phase
                              │
                  authorize and dispatch tool batch
                              │
                 checkpoint complete pending results
                              │
                    project results into history
                              │
                       step edits / checkpoint
                              └──────────→ next model request
```

A model response is committed before tool dispatch. Tool results retain call
order even when compatible calls execute concurrently. `max_exchanges` bounds
responses committed during this invocation; it is independent of HTTP retries.
A response without tool calls completes an ordinary run.

## Events and hooks

The loop publishes synchronous events at explicit boundaries. Subscribers run
inline on the publisher's execution path. They must finish promptly, must not
retain event references, and must not reenter the loop. Hooks share an ordered
session registry and are installed before runs begin.

`EditOnStepFinished` edits AgentInputState after committed tool results;
`EditOnRunFinished` edits it before the invocation's final outcome is delivered.
The loop validates edits and enforces its rollback contract. These events are
useful for pruning history and updating persistent context statistics. An event
is not a license to mutate arbitrary state asynchronously.

## Cancellation and failures

The long model request is interruptible. A tool invocation is allowed to settle
because it may have external effects. Cancellation that wins while confirmation
is pending denies that call. Calls not dispatched can receive explicit skipped
results; dispatched results remain visible before the next invocation.

Neither cancellation nor a failed run promises rollback of a filesystem write,
remote request, or child process effect. An observer exception can occur after
state has advanced. Recovery therefore uses committed progress and its phase,
not only a success/failure flag.

## Recovery phases

| Phase | Meaning after interruption |
| --- | --- |
| `ready` | Settled boundary from which a suitable request can proceed |
| `model` | Model work was pending; recovery must not invent a response |
| `tools` | Tool dispatch may have produced external effects; unsafe replay is blocked |
| `projection` | Complete pending results can be projected without repeating tools |
| `blocked` | Automatic progression is refused pending external investigation |

`committed_response_sequence` is a persistent monotonic response-commit counter.
It survives history pruning and separate invocations. It is neither a WebSocket
sequence number nor a Hub replay cursor.

The [wire contract](../core/worker-protocol.md#cancellation-shutdown-and-recovery)
defines how clients observe these states and which continuation requests the
worker accepts.
