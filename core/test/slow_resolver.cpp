// Test-only interposition of an already-running system resolver backend.
#include <condition_variable>
#include <mutex>
#include <cstring>
#include <dlfcn.h>
#include <netdb.h>

namespace {
std::mutex mutex;
std::condition_variable changed;
bool entered = false;
bool released = false;
}
extern "C" void resolver_reset() {
    std::lock_guard lock(mutex);
    entered = released = false;
}
extern "C" void resolver_wait_entered() {
    std::unique_lock lock(mutex);
    changed.wait(lock, [] { return entered; });
}
extern "C" void resolver_release() {
    std::lock_guard lock(mutex);
    released = true;
    changed.notify_all();
}
extern "C" int getaddrinfo(const char* host, const char* service,
                           const addrinfo* hints, addrinfo** result) {
    using Function = int (*)(const char*, const char*, const addrinfo*, addrinfo**);
    static auto real = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "getaddrinfo"));
    if (host && std::strcmp(host, "controlled-resolution.invalid") == 0) {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [] { return released; });
        host = "127.0.0.1";
    }
    return real(host, service, hints, result);
}
