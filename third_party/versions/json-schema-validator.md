# pboettch/json-schema-validator

- [Upstream release 2.4.1](https://github.com/pboettch/json-schema-validator/releases/tag/2.4.1)
- Source archive SHA-256: `0c8bf7ba59bd50e312d82da4bab1d49cf3e2211cfbd659472f9c90505d2a4e4c`
- Local artifacts: `include/nlohmann/json-schema.hpp` and `libs/libnlohmann_json_schema_validator.a` beneath `SIMPLEX_THIRDPARTY_DIR`.
- [MIT license and bundled component notice](../license/json-schema-validator/)

After installing the pinned nlohmann/json header, run the following from the
repository root with the worker's C++ toolchain. The helper verifies the
archive and builds a position-independent static library for that target.

```sh
CXX=g++-14 JOBS=1 bash docker/install-json-schema-validator.sh third_party
```

The intrinsic tool declaration loader uses the embedded Draft 7 meta-schema
at startup. The installed worker needs no external schema files for this check.
