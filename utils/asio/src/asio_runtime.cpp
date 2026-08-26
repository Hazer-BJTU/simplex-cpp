// asio_runtime.cpp — the single translation unit that compiles the
// Boost.Asio runtime for the whole process.
//
// Boost.Asio is header-only by default: every module including its headers
// compiles a private copy of the scheduler / epoll reactor / SSL engine AND —
// the part that bites — of their per-module static state. The sharpest edge
// is call_stack<Key>::top_, a template-static thread_local: each shared
// object instantiates its own. A host executable and a dlopened plugin that
// each carry a copy then share one io_context across the boundary — the
// plugin's reactor callback (descriptor_state::do_complete) runs on the
// host's scheduler thread, calls scheduler::compensating_work_started(), and
// reads the PLUGIN's empty top_. The null check is compiled out by
// BOOST_ASIO_ASSUME("only called from inside scheduler"), so the empty stack
// becomes a null dereference — intermittent, because only the reactor's
// compensate branch takes that path. (Observed live: 2 of 5 DeepSeek
// exchanges segfaulted at compensating_work_started+41 inside
// libllm_deepseek.so while io_context::run() lived in the host.)
//
// Defining BOOST_ASIO_SEPARATE_COMPILATION turns every asio header into
// extern declarations, and compiling impl/src.hpp (+ the SSL runtime) here
// produces the single defining copy. Shipped as a SHARED library, the loader
// guarantees one runtime per process by SONAME — exactly the medicine
// eventbus_lib applies to default_bus(). The macro travels as a usage
// requirement of asio_iface, so consumers cannot forget it; a module that
// links asio headers some other way silently falls back to header-only
// semantics and re-splits the runtime.

#define BOOST_ASIO_SEPARATE_COMPILATION

#include <boost/asio/ssl.hpp>  // config + SSL surface (OpenSSL via openssl_iface)

#include <boost/asio/impl/src.hpp>
#include <boost/asio/ssl/impl/src.hpp>
