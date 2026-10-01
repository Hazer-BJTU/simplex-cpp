# Plugin development workflow

Start inside the repository build so host and modules share the generated
version headers, compile definitions, compiler, Boost runtime, and support
libraries. The installed worker archive is a runtime distribution, not a
standalone plugin SDK.

## Choose a domain and starting point

| Domain | Working source example |
| --- | --- |
| Tools | [noop toolset](https://github.com/Hazer-BJTU/simplex-cpp/tree/main/tools/extensions/stubs/noop) |
| Loop hooks | [noop hook](https://github.com/Hazer-BJTU/simplex-cpp/tree/main/loop/extensions/stubs/noop) |
| Models | [DeepSeek provider](https://github.com/Hazer-BJTU/simplex-cpp/tree/main/llm/providers/deepseek) |

Copy an example into a new package, choose a unique descriptor name, and add its
subdirectory to the corresponding CMake build. Build a `MODULE`, not an
executable. Set the domain's plugin output directory so install rules can
collect the module.

## Export the contract

Each domain exports a descriptor factory and a product factory with exact
signatures. Export the aliases using `BOOST_DLL_ALIAS` and emit
`SIMPLEX_EXPORT_PLUGIN_MAGIC` once per module. Use externally linked functions
in a named namespace. Report the domain ABI version from generated versioning
constants rather than copying a numeric literal.

Descriptor names, product names, and YAML names must agree. Factories validate
options before exposing a partially configured object. Follow the generic
loader's exception contract and use `std::exception`-derived diagnostics.

## Test the whole lifecycle

A useful test exercises the actual shared library:

1. Discover its descriptor and construct from installed-format YAML.
2. Register it with a session registry or model dispatcher.
3. Invoke its behavior with valid and invalid inputs.
4. Destroy the loader/registry while retaining a permitted handle, then release
   that handle to verify lifetime ownership.
5. Check malformed configuration, missing aliases, wrong identity, and wrong ABI.

For tools, test authorization attributes, exceptions, and partial external
effects. For hooks, test disconnection and edit validation. For providers, use a
local HTTP fixture to test request translation, responses, retries, and errors
without requiring live credentials.

Use the [build guides](../building/docker.md) to compile host and plugin in one
context. Verify `cmake --install` places both the module and configuration in the
expected layout. Restart the worker after deploying changed code or YAML.

## Evolving the interface

Binary-incompatible contract changes require the corresponding ABI version in
[versioning/CMakeLists.txt](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/versioning/CMakeLists.txt)
to change and affected modules to be rebuilt. Do not bypass the admission gate
or statically embed separate copies of the shared runtime into each plugin.
