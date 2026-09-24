# File IO utilities

Link `fileio_lib` and include `fileio/atomic_write.hpp` to write a destination
through a synchronous stream callback:

```cpp
#include "fileio/atomic_write.hpp"
#include <ostream>

fileio::atomic_write("./data/report.txt", [](std::ostream& output) {
    output << "Complete report\n";
});
```

The utility accepts arbitrary bytes and has no dependency on `load`, dataclasses,
JSON, or logging. POSIX descriptor ownership, temporary-file cleanup, and buffered
stream output are private implementation details in `src/atomic_write.cpp`.
Application code supplies only its rendering operation and destination.

Missing parent directories are created. An exclusively created, mode-0600
temporary file in the destination directory receives the output. The utility
flushes, syncs, and closes it before rename, then syncs the parent directory.
Callback exceptions propagate unchanged. A stream failure or a callback failure
does not publish partial contents; the temporary file is removed during exception
unwinding. An empty callback is rejected before filesystem changes.

Replacement overwrites destination permissions with mode 0600 and replaces a
destination symlink instead of following it. If syncing the parent fails after
rename, the error identifies that replacement has already occurred. Newly
created ancestor directories are not individually synced. Forced termination
may leave `.simplex-write-*` temporary files; there is no automatic scavenger.
Callers must coordinate simultaneous writers to the same file and serialize the
callback's reads with mutations of any data being exported.

The release installer includes the shared utility library through the project's
normal target discovery. `test_atomic_write` checks binary output spanning buffer
boundaries, empty/replacement writes, exception cleanup, stream failure, rename
failure, and symlink replacement independently of application serialization.
