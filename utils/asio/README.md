# asio — the process-wide Boost.Asio runtime

`utils/asio` exists for one reason: **Boost.Asio must be compiled exactly once
per process**, and this module is that once.

## Why

Asio is header-only by default. Every module that includes its headers
compiles a private copy of the scheduler, the epoll reactor, the SSL engine —
and of their per-module static state. The dangerous piece is
`call_stack<Key>::top_`, a template-static `thread_local`: each shared object
instantiates its own copy.

The plugin architecture shares one `io_context` across a `dlopen` boundary:
the host executable creates it, provider plugins run their async chains on
it. With two compiled runtimes in the process:

1. the host's `io_context::run()` pushes the thread's context onto the
   **host's** `top_`;
2. a plugin-compiled reactor callback (`descriptor_state::do_complete`, whose
   function pointer lives in the plugin) later runs on that same thread and
   calls `scheduler::compensating_work_started()`, which reads the
   **plugin's** `top_` — empty;
3. `BOOST_ASIO_ASSUME` promises that pointer non-null ("only called from
   inside scheduler"), so the null check is compiled out in Release, and the
   empty stack becomes a null dereference.

The crash is intermittent — only the reactor's compensate branch (a completed
epoll event with no completed user operation) takes that path. Observed live:
2 of 5 DeepSeek exchanges segfaulted, always at
`compensating_work_started+41` inside `libllm_deepseek.so`.

This is the same class of bug as an inline `default_bus()` singleton forking
per module; the fix is the same medicine `eventbus_lib` applies.

## What ships

- `asio_lib` (SHARED, `libasio.so`): one TU compiling
  `boost/asio/impl/src.hpp` + `boost/asio/ssl/impl/src.hpp` under
  `BOOST_ASIO_SEPARATE_COMPILATION`. The loader guarantees one runtime per
  process by SONAME.
- `asio_iface` (INTERFACE): the consumption target. Carries Boost headers,
  OpenSSL (asio's config must detect the same OpenSSL in every consumer),
  the shared library, and the `BOOST_ASIO_SEPARATE_COMPILATION` definition —
  all as usage requirements, so a consumer cannot forget the switch.

## Rule

**Link `asio_iface` — never raw `Boost::headers` — in any target that
includes asio headers.** A module that compiles asio without the switch
silently falls back to header-only semantics and re-splits the runtime in
that process. Currently wired through `endpoint_lib`, `llm_iface`, and
`indextools_lib` (all PUBLIC), which covers every asio consumer in the tree.

## Dependencies

- Boost headers (`Boost::headers`, top-level find_package)
- OpenSSL (`openssl_iface`) — for the SSL runtime and consistent
  `BOOST_ASIO_HAS_OPENSSL`-era config across consumers
