# logging

A module of the `utils` package: a thread-safe, `std::format`-based console
logger (`logging::Logger`) shared across the whole `simplex-cpp` project.

## Layout

```
utils/logging/
├── CMakeLists.txt        # builds logging_lib
├── README.md             # this file
├── include/logging/
│   └── logger.hpp        # public header — #include "logging/logger.hpp"
├── src/
│   └── logger.cpp        # Logger implementation (static state, formatting, I/O)
└── test/
    └── test_logger.cpp   # unit tests
```

## Consuming

Link `logging_lib`. Its PUBLIC include dir brings `logging/logger.hpp`, and the
static objects provide `Logger`'s out-of-line definitions.

```cmake
target_link_libraries(my_target PRIVATE logging_lib)
```

```cpp
#include "logging/logger.hpp"

logging::Logger::info("Listening on port {}", 8080);
logging::Logger::error("Failed to open '{}': {}", path, ec.message());
```

## Dependencies

The C++ standard library and, on Unix, POSIX thread signal masks. The
default minimum level is `LogLevel::info` in release builds and `LogLevel::debug`
in debug builds (guarded by `DEBUG_BUILD`, defined on `logging_lib` in Debug
config).

## Disconnected console readers

Each log line is flushed during its synchronous write. On Unix, the writer
blocks SIGPIPE on the calling thread, consumes a newly generated signal only
when the write reports EPIPE, and restores the original mask. Existing pending
signals and global dispositions are preserved. This lets a worker survive a
Hub crash that closes its log pipes and reconnect to a replacement Hub. Closed
streams retain their normal error state; discarded log output is not replayed.
