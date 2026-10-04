<p align="center">
  <img src="assets/simplex-logo-v4.svg" alt="Simplex — C++ agent harness" width="520">
</p>

<p align="center">
  <a href="https://github.com/Hazer-BJTU/simplex-cpp/actions/workflows/ci.yml"><img src="https://github.com/Hazer-BJTU/simplex-cpp/actions/workflows/ci.yml/badge.svg" alt="CI status"></a>
  <a href="https://github.com/Hazer-BJTU/simplex-cpp/actions/workflows/docs.yml"><img src="https://github.com/Hazer-BJTU/simplex-cpp/actions/workflows/docs.yml/badge.svg" alt="Documentation build and deployment status"></a>
  <a href="https://github.com/Hazer-BJTU/simplex-cpp/releases/latest"><img src="https://img.shields.io/github/v/release/Hazer-BJTU/simplex-cpp" alt="Latest release"></a>
  <a href="https://www.npmjs.com/package/@hazer-bjtu/simplex-hub"><img src="https://img.shields.io/npm/v/%40hazer-bjtu%2Fsimplex-hub" alt="npm version"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/Hazer-BJTU/simplex-cpp" alt="License"></a>
</p>

<p align="center">
  <a href="https://isocpp.org/"><img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&amp;logoColor=white" alt="C++20"></a>
  <a href="https://www.boost.org/"><img src="https://img.shields.io/badge/Boost-C%2B%2B%20Libraries-00599C" alt="Boost C++ libraries"></a>
  <a href="https://nodejs.org/"><img src="https://img.shields.io/badge/Node.js-22.18%2B-5FA04E?logo=nodedotjs&amp;logoColor=white" alt="Node.js 22.18 or newer"></a>
  <a href="https://www.typescriptlang.org/"><img src="https://img.shields.io/badge/TypeScript-3178C6?logo=typescript&amp;logoColor=white" alt="TypeScript"></a>
  <a href="https://react.dev/"><img src="https://img.shields.io/badge/React-20232A?logo=react&amp;logoColor=61DAFB" alt="React"></a>
</p>

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
