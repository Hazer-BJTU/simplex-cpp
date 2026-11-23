#!/usr/bin/env bash
# Build the pinned validator against the same nlohmann/json header as the worker.
set -euo pipefail

prefix=$(cd "${1:?usage: install-json-schema-validator.sh THIRD_PARTY_DIR}" && pwd)
test -f "$prefix/include/nlohmann/json.hpp"
version=2.4.1
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
cd "$temporary"

wget --quiet --timeout=30 --tries=3 -O source.tar.gz \
    "https://codeload.github.com/pboettch/json-schema-validator/tar.gz/refs/tags/$version"
echo '0c8bf7ba59bd50e312d82da4bab1d49cf3e2211cfbd659472f9c90505d2a4e4c  source.tar.gz' | sha256sum -c -
tar -xzf source.tar.gz

# The upstream subproject accepts an existing nlohmann_json target. Supply the
# installed single header so CMake never discovers or downloads a second copy.
cat > CMakeLists.txt <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(simplex_validator_dependency LANGUAGES CXX)
add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
target_include_directories(nlohmann_json INTERFACE "${THIRDPARTY_PREFIX}/include")
add_subdirectory("${VALIDATOR_SOURCE}" validator)
CMAKE

cmake -S . -B build \
    -DTHIRDPARTY_PREFIX="$prefix" -DVALIDATOR_SOURCE="json-schema-validator-$version" \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_CXX_STANDARD=20 \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DJSON_VALIDATOR_SHARED_LIBS=OFF
cmake --build build --target nlohmann_json_schema_validator --parallel "${JOBS:-1}"
install -m 644 "json-schema-validator-$version/src/nlohmann/json-schema.hpp" "$prefix/include/nlohmann/"
install -d "$prefix/libs"
install -m 644 build/validator/libnlohmann_json_schema_validator.a "$prefix/libs/"
