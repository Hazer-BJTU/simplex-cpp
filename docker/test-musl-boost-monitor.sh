#!/usr/bin/env bash
# Exercise Boost.Test on a simulated host whose signal frame exceeds musl's
# fixed SIGSTKSZ. The interposer is confined to these disposable test processes.
set -euo pipefail
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

BOOST_AUTO_TEST_CASE(pass) { BOOST_TEST(true); }
BOOST_AUTO_TEST_CASE(assertion_failure) { BOOST_TEST(false); }
BOOST_AUTO_TEST_CASE(signal_failure) { std::raise(SIGSEGV); }
CPP
cc -shared -fPIC "$temporary/large-frame.c" -o "$temporary/large-frame.so" -ldl
g++ "$temporary/monitor.cpp" -o "$temporary/monitor" -lboost_unit_test_framework
run_monitor() {
    env LD_PRELOAD="$temporary/large-frame.so" \
        "$temporary/monitor" --run_test="$1" > "$temporary/output" 2>&1
}
if ! run_monitor pass; then
    cat "$temporary/output" >&2
    exit 1
fi
for failing in assertion_failure signal_failure; do
    if run_monitor "$failing"; then
        echo "test failure was masked: $failing" >&2
        exit 1
    fi
    if grep -q 'Test setup error:' "$temporary/output"; then
        cat "$temporary/output" >&2
        exit 1
    fi
done
echo 'musl Boost.Test large signal-frame regression passed'
