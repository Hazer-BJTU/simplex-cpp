#!/usr/bin/env bash
# Exercise Boost.Test on a simulated host whose signal frame exceeds musl's
# fixed SIGSTKSZ. The interposer is confined to these disposable test processes.
set -euo pipefail
stage=${1:-/opt/simplex}
stage=$(cd -- "$stage" && pwd)
# Exercise the installed library as well as the build library. An absent bundle
# must fail instead of falling back to /usr/local/lib in the build container.
loader="$stage/lib/ld-musl-x86_64.so.1"
test -x "$loader"
shopt -s nullglob
libraries=("$stage"/lib/libboost_unit_test_framework.so*)
(( ${#libraries[@]} > 0 ))
for library in "${libraries[@]}"; do test -s "$library"; done
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
cat > "$temporary/large-frame.c" <<'C'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <sys/auxv.h>

unsigned long getauxval(unsigned long type) {
    unsigned long (*real_getauxval)(unsigned long) = dlsym(RTLD_NEXT, "getauxval");
    if (type == AT_MINSIGSTKSZ) return 65536;
    return real_getauxval(type);
}

int sigaltstack(const stack_t *requested, stack_t *previous) {
    int (*real_sigaltstack)(const stack_t *, stack_t *) = dlsym(RTLD_NEXT, "sigaltstack");
    if (requested && !(requested->ss_flags & SS_DISABLE) && requested->ss_size < 65536) {
        errno = ENOMEM;
        return -1;
    }
    return real_sigaltstack(requested, previous);
}
C
cat > "$temporary/monitor.cpp" <<'CPP'
#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE musl_signal_stack_regression
#include <boost/test/unit_test.hpp>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <link.h>
#include <string>

// Verify loader resolution, not just the presence of a similarly named file.
BOOST_AUTO_TEST_CASE(bundled_library) {
    const char* expected = std::getenv("SIMPLEX_EXPECT_BOOST_DIRECTORY");
    if (!expected) return;
    bool found = false;
    struct Context { const char* expected; bool* found; } context{expected, &found};
    dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* opaque) {
        auto& state = *static_cast<Context*>(opaque);
        const std::filesystem::path path(info->dlpi_name);
        if (path.filename().string().starts_with("libboost_unit_test_framework.so")) {
            *state.found = std::filesystem::canonical(path).parent_path()
                == std::filesystem::canonical(state.expected);
            return 1;
        }
        return 0;
    }, &context);
    BOOST_TEST(found);
}

BOOST_AUTO_TEST_CASE(pass) { BOOST_TEST(true); }
BOOST_AUTO_TEST_CASE(assertion_failure) { BOOST_TEST(false); }
BOOST_AUTO_TEST_CASE(signal_failure) { std::raise(SIGSEGV); }
CPP
cc -shared -fPIC "$temporary/large-frame.c" -o "$temporary/large-frame.so" -ldl
g++ -std=c++20 "$temporary/monitor.cpp" -o "$temporary/monitor" -lboost_unit_test_framework
run_monitor() {
    if [[ "$mode" == bundled ]]; then
        env -u LD_LIBRARY_PATH LD_PRELOAD="$temporary/large-frame.so" \
            SIMPLEX_EXPECT_BOOST_DIRECTORY="$stage/lib" \
            "$loader" --library-path "$stage/lib" "$temporary/monitor" \
            --run_test="$1" > "$temporary/output" 2>&1
    else
        env -u SIMPLEX_EXPECT_BOOST_DIRECTORY -u LD_LIBRARY_PATH \
            LD_PRELOAD="$temporary/large-frame.so" \
            "$temporary/monitor" --run_test="$1" > "$temporary/output" 2>&1
    fi
}
for mode in build bundled; do
    if ! run_monitor 'pass,bundled_library'; then
        cat "$temporary/output" >&2
        exit 1
    fi
    for failing in assertion_failure signal_failure; do
        if run_monitor "$failing"; then
            echo "test failure was masked ($mode): $failing" >&2
            exit 1
        fi
        if grep -q 'Test setup error:' "$temporary/output"; then
            cat "$temporary/output" >&2
            exit 1
        fi
    done
    echo "musl Boost.Test large signal-frame regression passed ($mode)"
done
