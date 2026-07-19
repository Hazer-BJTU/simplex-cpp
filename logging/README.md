# logging

A standalone global module: a thread-safe, `std::format`-based console logger
(`logging::Logger`) shared across the whole `simplex-cpp` project. It was split
out of `indextools/` so any module can log without linking the indextools host
library.

## Layout

```
logging/
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

`indextools_lib` links `logging_lib` PUBLIC, so anything linking `indextools_lib`
already gets the logger.

## Dependencies

Standard library only (`<chrono>`, `<format>`, `<mutex>`, `<ostream>`, …). The
default minimum level is `LogLevel::info` in release builds and `LogLevel::debug`
in debug builds (guarded by `DEBUG_BUILD`, defined on `logging_lib` in Debug
config, mirroring `indextools_lib`).
