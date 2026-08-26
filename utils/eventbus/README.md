# eventbus

A module of the `utils` package: a type-dispatched, synchronous
publish/subscribe bus (`eventbus::EventBus`) shared across the whole
`simplex-cpp` project. Modules react to each other's state changes
(session lifecycle, stream progress, ...) without knowing each other: an
event is a plain copyable struct, and its C++ type is the topic. Built on
`boost::signals2`, which the bus wraps with a `std::type_index` registry,
registration-order slot groups, and a never-lock-around-user-code
reentrancy discipline.

## Layout

```text
utils/eventbus/
├── CMakeLists.txt        # builds eventbus_lib (SHARED) + eventbus_iface
├── README.md             # this file
├── include/eventbus/
│   └── event_bus.hpp     # public header — #include "eventbus/event_bus.hpp"
├── src/
│   └── default_bus.cpp   # the one definition of default_bus() (the singleton)
└── test/
    ├── CMakeLists.txt
    └── test_event_bus.cpp  # unit tests
```

## Consuming

Link `eventbus_iface`; its INTERFACE include dir brings
`eventbus/event_bus.hpp` and it links `eventbus_lib` (SHARED) — the
compiled home of the `default_bus()` singleton.

```cmake
target_link_libraries(my_target PRIVATE eventbus_iface)
```

```cpp
#include "eventbus/event_bus.hpp"

// 1. Define an event: plain copyable struct, no base class.
struct SessionStarted {
    std::string id;
};

// 2. Subscribe — event type spelled explicitly, slot is any callable
//    taking const SessionStarted&. ScopedSubscription disconnects on
//    scope exit; keep the returned Connection instead if you want to
//    unsubscribe by hand later.
eventbus::EventBus::ScopedSubscription sub =
    eventbus::default_bus().subscribe<SessionStarted>(
        [&](const SessionStarted& e) { /* react to e.id */ });

// 3. Publish — every matching slot runs here, inline, in the order it
//    subscribed.
eventbus::default_bus().publish(SessionStarted{.id = "s1"});
```

Construct your own `eventbus::EventBus` when the bus's lifetime should
follow a component rather than the process; `default_bus()` is the
process-wide shared instance.

## Contract

- **Synchronous**: `publish()` returns after the last slot. No queues, no
  worker threads.
- **Ordering**: slots for one event type run in registration order;
  re-subscribing after a disconnect goes to the back.
- **Threading**: every operation is safe to call concurrently from any
  thread, including from inside a running slot (the registry lock is
  never held while user code runs). Slots run on the publishing thread.
- **Exceptions**: a throwing slot propagates unchanged to the publisher —
  never swallowed, never logged. Slots after the thrower for that event
  are not invoked.
- **`clear()`**: disconnects everything; a concurrent in-flight
  `publish()` finishes against the pre-clear slot set.
- **Default bus**: the singleton is deliberately NOT inline — it lives in
  `eventbus_lib` (SHARED), so executables and dlopened plugins alike bind
  to one bus per process by SONAME, and a module that forgets to link it
  fails loudly at link time instead of silently publishing into a private
  second bus. Initialised on first use, destroyed during static
  destruction — long-lived subscribers on it should disconnect explicitly
  before `main` returns rather than rely on scope handles.

## Dependencies

Boost.Signals2 headers only (via the project's single top-level
`find_package(Boost)` — `Boost::headers`), plus one compiled TU for the
shared singleton. Deliberately no `logging_lib` dependency: slot
exceptions are the publisher's problem, and the bus stays usable from
anywhere, including before logger setup.
