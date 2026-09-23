# Built-in loop hooks

This package will contain hooks linked directly into the host. Each concrete
hook derives from `loop::LoopHookInterface` in `loop/hook_interface.hpp` and
subscribes to the explicit synchronous `eventbus::EventBus` passed at attach
time. It does not use `extensions` declarations or dynamic loading.

Keep one directory per hook family when implementations are added, following
the `tools/intrinsic/toolsets/` layout. The common interface and binding
lifetime belong to the top-level `loop` package, so built-in and future
external hooks use the same contract.
