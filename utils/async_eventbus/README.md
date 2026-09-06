# async_eventbus

A module of the `utils` package: the asynchronous counterpart of
`utils/eventbus`. It is a type-dispatched, serial-async publish/subscribe bus
(`eventbus::AsyncEventBus`) shared across the whole `simplex-cpp` project. An
event is a struct whose C++ type is the topic — it must derive from
`eventbus::AsyncEventBase` (the base supplies the `errors` container that
capture-mode publish fills); the difference is the dispatch model — handlers
are Boost.Asio coroutines, and `publish()` **folds** the event through them one
at a time instead of fanning it out.

## The fold model

```text
out = co_await handler_1(in);
out = co_await handler_2(out);
...
co_return out;
```

- The input `Evt` is **copied once** and passed to the first handler.
- Each handler's `co_return` value becomes the next handler's input.
- `publish()` returns the final `Evt`.

Most events only need a single field rewritten. A handler that must accumulate
a list result keeps a container (e.g. `std::vector`) **inside** the event and
pushes into it — no separate list-of-results return type is needed.

## Layout

```text
utils/async_eventbus/
├── CMakeLists.txt            # builds async_eventbus_lib (SHARED) + async_eventbus_iface
├── README.md                 # this file
├── include/eventbus/
│   └── async_event_bus.hpp   # public header — #include "eventbus/async_event_bus.hpp"
├── src/
│   └── default_async_bus.cpp # the one definition of default_async_bus()
└── test/
    ├── CMakeLists.txt
    └── test_async_event_bus.cpp  # unit tests
```

## Consuming

Link `async_eventbus_iface`; it carries the include dir and links
`asio_iface` (the one-compiled-copy-of-asio rule) plus `async_eventbus_lib`
(SHARED) — the compiled home of the `default_async_bus()` singleton.

```cmake
target_link_libraries(my_target PRIVATE async_eventbus_iface)
```

```cpp
#include "eventbus/async_event_bus.hpp"

struct SessionStarted : eventbus::AsyncEventBase {
    std::string id;
    std::vector<std::string> log;
};

eventbus::AsyncEventBus::ScopedSubscription sub =
    eventbus::default_async_bus().subscribe<SessionStarted>(
        [](const SessionStarted& e) -> boost::asio::awaitable<SessionStarted> {
            SessionStarted out = e;
            out.log.push_back("handled");
            co_return out;                      // becomes the next input
        });

// Inside a coroutine:
SessionStarted ev;
ev.id = "s1";
SessionStarted result = co_await eventbus::default_async_bus().publish(ev);
```

Construct your own `eventbus::AsyncEventBus` when the bus's lifetime should
follow a component rather than the process.

## Contract

- **Serial fold**: `publish()` co_awaits every live handler for the event type
  in registration order; each handler's `co_return` value becomes the next
  handler's argument. No queues, no worker threads, no concurrency between
  handlers of one publish.
- **Ordering**: handlers for one event type run in registration order;
  re-subscribing after a disconnect goes to the back.
- **Payload**: `publish(const Evt&)` copies the event and passes the copy by
  const reference to the first handler; the caller's event is never mutated.
- **No subscribers**: `publish()` returns a copy of the input unchanged.
- **Event type**: `Evt` must derive from `eventbus::AsyncEventBase`; the
  `AsyncEventType` concept enforces this at compile time.
- **Exceptions** (two modes, token-selected, one unified return type):
  - `publish(evt)` / `publish(evt, propagate_exceptions)` — a throwing handler
    propagates unchanged and the remaining handlers are not invoked.
  - `publish(evt, capture_exceptions)` — every handler runs; each throwing
    handler's exception is collected and the fold continues with the value
    that was current before the throw. The returned event's
    `AsyncEventBase::errors` is the input's pre-existing errors (preserved,
    never silently discarded) plus one `std::exception_ptr` per throw, in
    order. `errors` is bus-owned, final publisher-facing metadata: the bus
    writes it only when the fold completes, so it survives a handler that
    returns a freshly constructed event, and handlers do not observe this
    run's captured errors mid-fold.
- **Threading**: `subscribe` / `subscriber_count` / `clear` are safe to call
  concurrently from any thread, including from inside a running handler. The
  registry mutex is never held across a `co_await`. Concurrent publishes are
  independent — each folds serially on its own.
- **Disconnect**: stops future dispatches from starting that handler; an
  in-flight handler runs to completion. Dead slots are pruned lazily on the
  next subscribe and all at once on `clear()`.
- **Default bus**: the singleton is deliberately NOT inline — it lives in
  `async_eventbus_lib` (SHARED), so executables and dlopened plugins bind to
  one bus per process by SONAME. Initialised on first use, destroyed during
  static destruction — long-lived subscribers on it should disconnect
  explicitly before `main` returns rather than rely on scope handles.

## Dependencies

Boost.Asio via `asio_iface` (which provides the one compiled Asio runtime,
`BOOST_ASIO_SEPARATE_COMPILATION`, and the include dir), plus one compiled TU
for the shared singleton. Deliberately no `logging_lib` dependency: handler
exceptions are the publisher's problem, and the bus stays usable from anywhere,
including before logger setup.
