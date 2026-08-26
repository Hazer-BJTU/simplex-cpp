# =============================================================================
# asio — module (utils package)
#
# The one compiled copy of the Boost.Asio runtime that every executable AND
# every dlopened plugin in the process binds to. Header-only Asio compiles a
# private runtime (scheduler / epoll reactor / SSL engine / their per-module
# template-static thread_local state) into each module; an io_context shared
# across a dlopen boundary then splits that state and the plugin's reactor
# callbacks crash intermittently reading an empty thread call stack (see
# src/asio_runtime.cpp for the full post-mortem).
#
# `asio_lib` (SHARED) compiles impl/src.hpp + the SSL runtime once;
# `asio_iface` is the consumption target: Boost headers, OpenSSL (asio's
# config must see the same OpenSSL everywhere), the shared library, and the
# BOOST_ASIO_SEPARATE_COMPILATION switch — all usage requirements. Linking
# asio_iface is the only sanctioned way to consume Boost.Asio here.
# =============================================================================
add_library(asio_lib SHARED src/asio_runtime.cpp)
set_target_properties(asio_lib PROPERTIES OUTPUT_NAME asio)
target_link_libraries(asio_lib PRIVATE Boost::headers openssl_iface)
target_compile_options(asio_lib PRIVATE
    $<$<CONFIG:Debug>:-g -Wall -Wextra -O0>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>)

add_library(asio_iface INTERFACE)
target_compile_definitions(asio_iface INTERFACE BOOST_ASIO_SEPARATE_COMPILATION)
target_link_libraries(asio_iface INTERFACE Boost::headers openssl_iface asio_lib)
