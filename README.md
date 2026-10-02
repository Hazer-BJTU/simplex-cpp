<p align="center">
  <img src="assets/simplex-logo-v4.svg" alt="Simplex — C++ agent harness" width="520">
</p>

[![CI](https://github.com/Hazer-BJTU/simplex-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/Hazer-BJTU/simplex-cpp/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Hazer-BJTU/simplex-cpp)](https://github.com/Hazer-BJTU/simplex-cpp/releases/latest)
[![npm](https://img.shields.io/npm/v/%40hazer-bjtu%2Fsimplex-hub)](https://www.npmjs.com/package/@hazer-bjtu/simplex-hub)
[![License](https://img.shields.io/github/license/Hazer-BJTU/simplex-cpp)](LICENSE)

**Understand every detail of an agent harness.**

A lightweight, native C++ harness framework for LLM agents—and a learning
project for building a minimal agent system from scratch.

[Documentation](https://hazer-bjtu.github.io/simplex-cpp/) ·
[Releases](https://github.com/Hazer-BJTU/simplex-cpp/releases/latest) ·
[Documentation source](docs/README.md) ·
[Contributing](CONTRIBUTING.md)

## Architecture

- **Worker:** one process per session, with an agent loop, model providers,
  tool and hook registries, cancellation, and persistent state.
- **Hub:** a separate Node.js service and browser panel for managing workers
  and interacting with them over WebSocket.
- **Extensions:** native model, tool, and loop-hook plugins with explicit
  interfaces and lifetime rules.

## Install

Download and extract the worker from [GitHub Releases](https://github.com/Hazer-BJTU/simplex-cpp/releases/latest).
Keep the extracted directory intact and add its `bin` directory to `PATH`.
The prebuilt worker requires Linux x86_64, glibc 2.34+, and OpenSSL 3.

Install the Hub with Node.js 22.18+:

```sh
npm install -g @hazer-bjtu/simplex-hub
export MODEL_API_KEY='your-api-key'
simplex-hub
```

Open <http://127.0.0.1:8800>. Configure a model and launch a worker using the
[deployment guide](https://hazer-bjtu.github.io/simplex-cpp/deployment/hub.html).

## Security

Native plugins execute with the worker's permissions; load only trusted code.
Local tools can execute commands and modify files. Workspace hints and tool
approval are not sandboxes, and cancellation does not undo completed effects.
Use restricted accounts or containers and protect Hub access.
[Security policy and private reporting](SECURITY.md) ·
[Security details](https://hazer-bjtu.github.io/simplex-cpp/architecture/security.html).
