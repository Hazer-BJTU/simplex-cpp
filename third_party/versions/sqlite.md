# SQLite

- Release: **3.54.0**, released on 2026-10-09.
- Version number: `3054000` (`SQLITE_VERSION_NUMBER`).
- Source ID (`SQLITE_SOURCE_ID` in both `sqlite3.c` and `sqlite3.h`):
  `2026-10-09 15:46:58 be8d059e9a49089ab2dce5ed26dd87aaf598fdc5fbe0b107c0dd758464bcd5e3`.
- [Release notes](https://www.sqlite.org/releaselog/3_54_0.html).
- [Official amalgamation archive](https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip).
- Archive SHA3-256 (not SHA-256):
  `7b670a62fdfbd672b75fef004cb703c8a3e87d3a5cc7d675b4a08337004a2d93`.
- [Public-domain notice](../license/sqlite/PUBLIC-DOMAIN.txt), based on
  [upstream's statement](https://www.sqlite.org/copyright.html).

## Local source layout and verification

Extract the archive's `sqlite-amalgamation-3540000/` contents into
`third_party/include/sqlite/`. This entire local directory is ignored by Git;
do not force-add its contents. Keep the source and headers from the same release.

The following files were checked byte-for-byte against the official archive:

| File | SHA-256 |
| --- | --- |
| `sqlite3.c` | `2da161e310ef0aba3465f22a4d12b8a43723236f0942fa76e8aac523c3317243` |
| `sqlite3.h` | `cd20a2877bc82b94e36c1093b727dd87cfa3c1d581aac6e240925535cbd90166` |
| `sqlite3ext.h` | `48963abfefb41ac329ef9d706acb5eadba952a99a748285c9ded6b5e0942721b` |
| `shell.c` | `c444573b69d70516e64df5ab38911f814ed5509020c92cc899bf1f6ffa582e8f` |

`sqlite3.c` is the C library implementation, and `sqlite3.h` is its public API.
`sqlite3ext.h` supports extension development. `shell.c` is the separate SQLite
CLI entry point and is not required to embed the library.

## Integration status

The [SQLite utilities](../../utils/sqlite/README.md) compile `sqlite3.c` as C
into `sqlite_runtime` (`libsimplex_sqlite3.so`) and provide a C++ RAII wrapper
through `sqlite_lib` (`libsimplex_sqlite.so`). Compilation uses
`SQLITE_THREADSAFE=1` and `SQLITE_OMIT_LOAD_EXTENSION=1`. CMake verifies the
source/header hashes; it does not fetch or fall back to a system SQLite.

The default source directory is `${SIMPLEX_THIRDPARTY_DIR}/include/sqlite`,
overridable with `SIMPLEX_SQLITE_SOURCE_DIR`. `docker/Dockerfile.build-base`,
`docker/Dockerfile.build-portable` and `docker/Dockerfile.build-musl` download
the pinned archive through
`docker/install-sqlite-amalgamation.sh` and verify its SHA3-256 before installing
these files at the default path.
Local source files remain excluded from the Docker build context. CI and worker
release workflows pin the published build-base, build-portable and build-musl
images by digest. Older images require a separately supplied source directory
for local verification.
Application persistence does not yet use SQLite.

The `test_sqlite` target checks runtime identity, copied text bindings,
transaction completion/rollback, error handling, connection lifetime, disk
reopening and write contention. These focused tests are not a full
cross-platform or durability qualification.
