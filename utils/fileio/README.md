# File IO utilities

`fileio/replace_existing.hpp` provides bounded reads and replacement of an
existing single-link regular file for text editing. It refuses the final
symlink and special mode bits, compares expected bytes before publication,
preserves owner/group/POSIX mode, and reports a conflict if the file changed.
Unlike `atomic_write`, it does not create parent directories and does not
replace a symlink. It does not preserve ACLs or extended attributes. The
replacement has the same published-versus-not-published directory-sync error
contract as `atomic_write`.

`fileio/read_prefix.hpp` provides `read_prefix(path, max_bytes)` for bounded
binary inspection of regular files. It follows symlinks, checks the opened
descriptor's type and uses nonblocking open so a FIFO does not wait for a
writer. Regular-file reads are synchronous, retry interrupted reads, stop at
EOF and return no more than `max_bytes`. A fixed-size scratch buffer grows the
returned string only as bytes arrive; a small file does not allocate its entire
maximum read allowance. The descriptor is close-on-exec and
closed on every exit. Errors throw; no text decoding or snapshot guarantee is
provided. `textedit` uses this primitive for advisory UTF-8 sampling.

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
close-on-exec temporary file in the destination directory receives the output. The utility
flushes, syncs, and closes it before rename, then syncs the parent directory.
Callback exceptions propagate unchanged. A stream failure or a callback failure
does not publish partial contents; the temporary file is removed during exception
unwinding. An empty callback is rejected before filesystem changes.

Replacement overwrites destination permissions with mode 0600 and replaces a
destination symlink instead of following it. If syncing the parent fails after
rename, `AtomicWriteError::published()` is true: the new file is visible but
crash durability is uncertain. POSIX failures retain their error code through
this `std::system_error` subclass. Other utility failures occur before publication;
callback exceptions retain their original type and belong to the callback. Newly
created ancestor directories are not individually synced. Forced termination
may leave `.simplex-write-*` temporary files; there is no automatic scavenger.
Callers must coordinate simultaneous writers to the same file and serialize the
callback's reads with mutations of any data being exported.

The release installer includes the shared utility library through the project's
normal target discovery. `test_atomic_write` checks binary output spanning buffer
boundaries, empty/replacement writes, exception cleanup, stream failure, rename
failure, symlink replacement, and close-on-exec descriptors independently of application serialization.

`test_persistence_publication` injects file and directory sync failures to verify
publication flags, destination contents, and propagation through the load API.
