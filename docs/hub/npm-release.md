# Hub npm package

The public package `@hazer-bjtu/simplex-hub` includes the Hub server,
configuration templates, compiled browser panel, and user documentation.
It requires Node.js 22.18 or newer. No separate panel build is needed.

```sh
npm install -g @hazer-bjtu/simplex-hub
simplex-hub --help
```

The C++ worker is installed separately. Use `simplex-hub install-worker` with
an explicit platform, or download a worker archive from GitHub Releases.
See [Installation](../getting-started/installation.md) for platform requirements,
worker installation, and starting the Hub.

## UI SDK

The package exposes the contract-only ESM entry point
`@hazer-bjtu/simplex-hub/ui`, including TypeScript declarations. It provides
public types and protocol constants for independent clients; it does not provide
a transport, state store, or server API. Importing it does not load the Hub server
or its runtime dependencies, and browser consumers need no Node polyfills.

The SDK shares the Hub package version. Compatibility with a running Hub is
established through protocol metadata and advertised capabilities, not by
assuming that matching npm versions imply matching remote capabilities.
See the [UI SDK guide](./ui-sdk.md) for the contract, client responsibilities,
and terminal/Web examples.
