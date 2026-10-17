/* Test-only publication barrier: never linked into installed targets. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int rename(const char* source, const char* destination) {
    int (*next)(const char*, const char*) = dlsym(RTLD_NEXT, "rename");
    const char* target = getenv("SIMPLEX_TEST_RENAME_TARGET");
    if (target && strcmp(destination, target) == 0) {
        const char* ready = getenv("SIMPLEX_TEST_READY_FD");
        const char* resume = getenv("SIMPLEX_TEST_RESUME_FD");
        if (ready && resume) {
            char byte = 'r';
            if (write(atoi(ready), &byte, 1) != 1 || read(atoi(resume), &byte, 1) != 1) {
                _exit(90);
            }
        }
        unsetenv("SIMPLEX_TEST_RENAME_TARGET");
    }
    return next(source, destination);
}
