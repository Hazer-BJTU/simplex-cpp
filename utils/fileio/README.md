# File IO utilities

`fileio/base64.hpp` converts arbitrary binary bytes between standard padded
Base64 and their original form. `base64_encode_file(path)` reads a regular file
and returns its Base64 text; `base64_decode_file(text, path)` validates the
encoding and atomically writes the decoded bytes. Neither function guesses a
media type or adds a data-URL prefix. Empty files produce empty strings.
Decoding rejects whitespace, URL-safe characters, malformed padding, and
nonzero unused bits. These convenience functions hold their input and result in
memory, so callers should set their own size limits before using them with
untrusted, potentially large files or strings. Reading does not guarantee a
snapshot if another process edits the file concurrently. The decoder inherits
`atomic_write`'s destination and publication behavior.

```cpp
#include "fileio/base64.hpp"

const auto encoded = fileio::base64_encode_file("image.png");
fileio::base64_decode_file(encoded, "image-copy.png");
```

`test_base64` compares encodings against OpenSSL, checks standard test vectors
and malformed input, and round-trips valid PNG, GIF, PDF, ZIP, and UTF-8 files
plus a larger arbitrary binary file.

`fileio/replace_existing.hpp` provides bounded reads and replacement of an
existing single-link regular file for text editing. It refuses the final
symlink and special mode bits, compares initially read identity and bytes before publication,
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

## Persistence paths, directory locks and session leases

`fileio/child_path.hpp` resolves a nonempty relative persistence child below a
root and rejects absolute paths, embedded NULs and parent traversal. Validation
is lexical only; a caller that needs a filesystem boundary must check it
separately.

`fileio/directory_tree_lock.hpp` takes a shared `flock` on every existing
ancestor and an exclusive one on the target directory. It requires a normalized
absolute path and refuses a symlinked ancestor (reported as `ELOOP` or
`ENOTDIR`) with an error naming the offending component, so a caller never
silently locks a different tree.

`fileio/session_lock.hpp` implements nonblocking advisory ownership of a stable
lock file. Exclusive mode opens it read-write and creates it; shared mode opens
it read-only, so inspection works on a readable session directory whose lock
file already exists, and creates it only when it does not exist yet. Errors name
the requested mode.

### Deferred atomic publication

`fileio::StagedWrite` renders and synchronizes a private same-directory file in
its constructor, then `publish()` performs the atomic rename and directory sync.
Destruction removes an unpublished temporary file. This lets a caller stage a
secondary artifact before an authoritative commit without exposing it on failure.
The two publications are not one transaction; callers must handle a postcommit
publication failure explicitly. `atomic_write()` uses the same implementation
for immediate publication. As with other filesystem operations, coroutine users
should construct, publish and destroy staged writes on their storage executor.
