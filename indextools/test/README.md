# Unit Testing Guide — indextools

This project uses **Boost.Test** as its unit-test framework. Tests live under
`test/`, are built with CMake + CTest, and run as part of the normal build
pipeline whenever `BUILD_TESTS=ON` (the default).

## Quick start

```bash
# Configure (tests are ON by default). Run from the project root:
cmake -B build -S .
cmake --build build --target test_lang -j$(nproc)

# Run all test cases
./build/bin/test_lang           # run directly
ctest --test-dir build --output-on-failure       # run via CTest
```

To **disable** tests (e.g. for a release-only build):

```bash
cmake .. -DBUILD_TESTS=OFF
```

## Directory layout

```
cpptools/
├── CMakeLists.txt          # top-level: finds Boost.Test, calls add_subdirectory(test)
├── test/
│   ├── CMakeLists.txt      # builds test_lang from *.cpp, registers with CTest
│   ├── README.md           # ← this file
│   └── test_lang.cpp       # example test suite (LangAnalyze abstract interface)
└── include/
    └── lang.hpp            # code under test
```

## Adding a new test file

1. Create `test/test_<component>.cpp`.
2. **No changes to `test/CMakeLists.txt` are needed** — `file(GLOB ...)` picks up
   every `.cpp` in `test/` automatically. Just drop the file in.
3. Re-run `cmake .. && make test_lang`.

### Minimal boilerplate

```cpp
// test/test_foo.cpp
#define BOOST_TEST_MODULE FooTests         // ① unique module name (once per .cpp)
#include <boost/test/unit_test.hpp>

#include "foo.hpp"                         // ② the header(s) you're testing

using namespace cpptools;

BOOST_AUTO_TEST_SUITE(FooSuite)            // ③ optional — groups related cases

BOOST_AUTO_TEST_CASE(some_descriptive_name)
{
    // arrange
    Foo foo(42);

    // act
    int result = foo.compute();

    // assert
    BOOST_CHECK_EQUAL(result, 84);
}

BOOST_AUTO_TEST_SUITE_END()
```

> **①** `BOOST_TEST_MODULE` must appear **exactly once per translation unit**,
> before the `#include <boost/test/unit_test.hpp>` line. It also defines
> `main()` for that test binary — you must **not** write your own `main`.

## Writing test cases

### Assertion macros

| Macro | Behaviour |
|-------|-----------|
| `BOOST_CHECK(expr)` | Non-fatal — continues on failure |
| `BOOST_REQUIRE(expr)` | **Fatal** — aborts the current test case |
| `BOOST_CHECK_EQUAL(a, b)` | Non-fatal equality check |
| `BOOST_REQUIRE_EQUAL(a, b)` | Fatal equality check |
| `BOOST_CHECK_THROW(expr, ExType)` | Passes if `expr` throws `ExType` |
| `BOOST_CHECK_NO_THROW(expr)` | Passes if `expr` does not throw |
| `BOOST_CHECK_CLOSE(a, b, tol%)` | Floating-point near-equality |

**Rule of thumb:**
- Use `BOOST_REQUIRE` when the *rest of the test would be meaningless* if the
  check fails (e.g. a pointer is null, a container has the wrong size).
- Use `BOOST_CHECK` for everything else — it lets you see *all* failures in one
  run instead of stopping at the first.

### Naming conventions

- **File name**: `test_<component>.cpp` (lowercase, underscores).
- **Test module**: `#define BOOST_TEST_MODULE <Component>Tests` — PascalCase,
  matches the component being tested.
- **Suite name**: `BOOST_AUTO_TEST_SUITE(<Component>Suite)` — groups related
  tests; one suite per header or class is typical.
- **Case name**: `BOOST_AUTO_TEST_CASE(verb_noun_description)` — lowercase +
  underscores, reads like a sentence. Good examples:
  - `open_loads_file_and_populates_source_and_lines`
  - `reset_clears_all_state`
  - `analyze_empty_source_produces_empty_result`

### Test structure: Arrange–Act–Assert

Keep the three phases visually distinct:

```cpp
BOOST_AUTO_TEST_CASE(reset_clears_all_state) {
    // Arrange
    TempFile tmp("a:1\nb:2\n");
    MockLangAnalyze analyzer;
    analyzer.open(tmp.path);
    analyzer.analyze();

    // Act
    analyzer.reset();

    // Assert
    BOOST_CHECK(analyzer.source().empty());
    BOOST_CHECK(analyzer.lines().empty());
    BOOST_CHECK(analyzer.result().empty());
}
```

## Testing abstract interfaces

When the class under test is purely abstract (like `cpptools::LangAnalyze`),
write a **minimal mock/fake implementation** inside an anonymous namespace in
the test file:

```cpp
namespace {

class MockFoo final : public cpptools::Foo {
    // ... implement every pure-virtual method with simple, predictable logic ...
};

} // anonymous namespace
```

The mock should be:
- **As simple as possible** — no more logic than needed to verify the interface
  contract.
- **Deterministic** — given the same input, always produce the same output.
- **Declared in the test file** — not in the production `include/` tree.

Use the mock to exercise every virtual method at least once. Multiple mock
implementations may be needed to cover different paths (e.g. success vs.
failure).

## RAII helpers for external resources

Temporary files, sockets, and other OS resources should be managed by RAII
wrappers in the anonymous namespace:

```cpp
struct TempFile {
    std::filesystem::path path;

    explicit TempFile(const std::string& content) {
        path = std::filesystem::temp_directory_path()
               / "cpptools_test_foo.txt";
        std::ofstream ofs(path);
        ofs << content;
    }

    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);   // ignore errors in dtor
    }
};
```

- Constructor creates the resource; destructor cleans it up.
- Destructors **must not throw** — swallow errors silently.
- Use `std::filesystem::temp_directory_path()` to avoid polluting the source
  tree.

## Working with the project's third-party libraries

The test target links against `Boost::unit_test_framework` and inherits the
same include directories as the main executable:

```cmake
target_include_directories(test_lang PRIVATE
    "${INDEXTOOLS_INCLUDE_DIR}"               # indextools headers
    "${SIMPLEX_THIRDPARTY_DIR}/include"       # nlohmann/json, tree-sitter, glob
)
```

(`INDEXTOOLS_INCLUDE_DIR` / `SIMPLEX_THIRDPARTY_DIR` are set by the
indextools module and the top-level CMakeLists respectively; see
`dependency.md`.)

If your test needs an additional library (e.g. `Boost::filesystem`), add it in
`test/CMakeLists.txt`:

```cmake
target_link_libraries(test_lang PRIVATE
    Boost::unit_test_framework
    Boost::filesystem               # ← added
)
```

## Conventions checklist

- [ ] One `BOOST_TEST_MODULE` per `.cpp` file.
- [ ] Test file is named `test_<component>.cpp`.
- [ ] Mock/fake classes live in an anonymous `namespace {}`.
- [ ] RAII wrappers manage temporary resources.
- [ ] `BOOST_REQUIRE` guards assertions the rest of the test depends on.
- [ ] `BOOST_CHECK` for non-critical assertions — let the test keep running.
- [ ] Clean build with `-Wall -Wextra` (no warnings).
- [ ] Both `./bin/test_lang` and `ctest` pass before committing.

## References

- [Boost.Test documentation](https://www.boost.org/doc/libs/release/libs/test/)
- [`test_lang.cpp`](./test_lang.cpp) — working example covering an abstract
  interface with 10 test cases.
