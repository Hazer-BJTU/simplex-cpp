# SQLite ownership utilities

`sqlite_lib` provides a small synchronous C++ API in `<sqlite/database.hpp>`.
It links one `sqlite_runtime` shared library built from the pinned C amalgamation.
Link consumers to `sqlite_lib`; SQLite headers and native handles are private to
this wrapper. The raw runtime target is available to dependency tests.

```cpp
sqlite::Database database("state.sqlite");
database.execute("CREATE TABLE IF NOT EXISTS notes(body TEXT)");
{
    sqlite::Transaction transaction(database);
    auto insert = database.prepare("INSERT INTO notes VALUES(?1)");
    insert.bind_text(1, "A note");
    insert.step();
    transaction.commit();
}
auto query = database.prepare("SELECT body FROM notes");
while (query.step() == sqlite::StepResult::Row) {
    const std::string body = query.column_text(0);
    // Consume an owning copy of the value.
}
```

## Ownership and execution

- `Database` opens an ordinary UTF-8 path read/write, creating the file when
  needed. `:memory:` is supported; parent directories are not created.
- `Statement` finalizes its statement on destruction. It retains the connection
  and copies bound text (`SQLITE_TRANSIENT`), including embedded NUL. Parameters
  use one-based indexes and result columns use zero-based indexes. A prepared
  statement executes once; another step after completion cannot rerun a write.
  Prepare accepts one statement followed by whitespace, with no trailing SQL or
  comments. `execute` supports trusted SQL batches and discards result rows.
- `Transaction` begins a deferred transaction and rolls back if not explicitly
  committed. Nested guards are rejected. A failed commit remains owned by the
  guard. If SQLite already rolled back implicitly, commit reports failure and
  rollback/destruction accepts the ended native transaction. Destructor rollback
  cannot throw; a connection still in a transaction after failed cleanup is
  marked unusable. Do not issue manual transaction/savepoint control while a
  guard exists.
- Objects are noncopyable and nonmovable. Statements and transaction guards can
  outlive the `Database` facade and keep the connection alive. Native resources
  stay within their shared-library implementation.
- All operations on a connection must be serialized by the caller for their
  full logical scope. `SQLITE_THREADSAFE=1` and `FULLMUTEX` protect SQLite calls,
  not an application sequence that spans multiple calls. No wrapper lock spans
  a coroutine suspension; the wrapper itself has no asynchronous API. Use an
  appropriate blocking executor for database work rather than an I/O loop.
- SQLite failures throw `sqlite::Error` with primary/extended codes. Diagnostics
  omit SQL, bound values and paths. Wrapper argument/state errors use standard
  C++ exceptions. Destructors finalize/close resources without throwing; complete
  statements and explicitly commit when their success must be observed.

This initial API has no statement reuse, BLOB binding, busy retry, cancellation,
schema migrations or application persistence policy. SQLite's default journal,
synchronization, foreign-key and busy settings apply; the owning application
must select those policies when storage integration is implemented.

## Build and validation

Populate the untracked source directory using the
[version record](../../third_party/versions/sqlite.md). CMake defaults to
`${SIMPLEX_THIRDPARTY_DIR}/include/sqlite`; override it with
`-DSIMPLEX_SQLITE_SOURCE_DIR=/absolute/path/to/sqlite` when dependencies and the
amalgamation live in different locations. CMake checks the pinned source/header
hashes, enables C and C++ LTO probing, and compiles only `sqlite3.c`. SQLite
extension loading is disabled; Simplex's own plugin mechanism is unaffected.
The two shared libraries use the existing release install rules and RPATHs.

Choose matching C/C++ compilers explicitly when toolchain aliases differ, for
example `-DCMAKE_C_COMPILER=/path/to/gcc` together with
`-DCMAKE_CXX_COMPILER=/path/to/g++`. The local default is `gcc-14`/`g++-14`;
the C compiler can also come from `CC` or a CMake toolchain file. Older build
images may map these two versioned commands to different GCC minor releases;
the updated `Dockerfile.build-base` aligns both to GCC 14.3.0.

After configuring the project, build `test_sqlite` with `-j1` and run
`ctest --test-dir BUILD_DIR -R '^test_sqlite$' --output-on-failure`.
Tests cover copied/empty/NUL-containing text, NULL, bounds, failure recovery,
transaction rollback, resource lifetimes, disk reopen and busy commit cleanup.

The updated `Dockerfile.build-base`, `Dockerfile.build-portable` and
`Dockerfile.build-musl` provision the source at the default path. The published
build-base, build-portable and build-musl images are pinned by digest in CI
and worker release workflows. Source files must not be committed as a
workaround.
