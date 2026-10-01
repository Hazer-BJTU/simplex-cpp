# Simplex

[![CI](https://github.com/Hazer-BJTU/simplex-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/Hazer-BJTU/simplex-cpp/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Hazer-BJTU/simplex-cpp)](https://github.com/Hazer-BJTU/simplex-cpp/releases/latest)
[![npm](https://img.shields.io/npm/v/%40hazer-bjtu%2Fsimplex-hub)](https://www.npmjs.com/package/@hazer-bjtu/simplex-hub)
[![License](https://img.shields.io/github/license/Hazer-BJTU/simplex-cpp)](LICENSE)

**让你清晰理解harness框架的每一个细节**

*Understand every detail of an agent harness.*

Simplex is a lightweight, native C++ harness framework for LLM agents. It is
also a learning project: a working system you can explore to understand how to
build a minimal agent from scratch, from the first user request to model calls,
tool execution, cancellation, and persistent conversation state.

## Architecture

Each worker runs as its own process. The C++ core connects model providers,
tool and hook registries, the agent loop, and disk persistence. Tools and hooks
can be built in or loaded as dynamic plugins; model providers share a common
interface.

Workers connect over WebSocket to a separate Node.js Hub. The Hub manages
worker sessions and exposes a browser panel for messages, tool approvals,
configuration, and conversation history. The documented protocol lets other
servers interact with the same worker independently of the bundled Hub.

## Install

Download and extract the worker archive from the
[latest GitHub Release](https://github.com/Hazer-BJTU/simplex-cpp/releases/latest).
The prebuilt worker supports Linux x86_64 with glibc 2.34+ and OpenSSL 3.
Keep the extracted directory intact and add its `bin` directory to your `PATH`:

```sh
export PATH="/path/to/extracted-worker/bin:$PATH"
simplex run --help
```

Install the Hub and its bundled browser panel with Node.js 22.18 or newer:

```sh
npm install -g @hazer-bjtu/simplex-hub
simplex-hub
```

Open <http://127.0.0.1:8800>. In **Configurations**, fill in the worker template
with your model provider, endpoint, and credentials, then create a session and
start its worker. The default local launch template uses `simplex run` from
`PATH`.

See the [documentation index](docs/README.md) for protocols and configuration,
and the [Hub guide](hub/README.md) for more usage details.
