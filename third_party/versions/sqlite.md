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

This record prepares the dependency for v1.0.x; it does not enable SQLite in
CMake, change persistence, or add a runtime library. Docker dependency images
will be handled separately. The local source directory is also excluded from
the Docker build context.

A local C compilation and an in-memory smoke check passed: header/runtime
version agreement, table creation, transactional insertion, prepared query and
clean statement/connection shutdown. This is not a cross-platform or durability
qualification.
