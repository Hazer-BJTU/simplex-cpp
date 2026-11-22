<p align="center">
  <img src="assets/simplex-logo-v4.svg" alt="Simplex — C++ agent harness" width="520">
</p>

<p align="center">
  <img src="https://github.com/Hazer-BJTU/simplex-cpp/actions/workflows/ci.yml/badge.svg" alt="CI status">
  <img src="https://github.com/Hazer-BJTU/simplex-cpp/actions/workflows/docs.yml/badge.svg" alt="Documentation build and deployment status">
  <img src="https://img.shields.io/github/v/release/Hazer-BJTU/simplex-cpp" alt="Latest release">
  <img src="https://img.shields.io/npm/v/%40hazer-bjtu%2Fsimplex-hub" alt="npm version">
  <img src="https://img.shields.io/github/license/Hazer-BJTU/simplex-cpp" alt="License">
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&amp;logoColor=white" alt="C++20">
  <img src="https://img.shields.io/badge/Boost-C%2B%2B%20Libraries-00599C" alt="Boost C++ libraries">
  <img src="https://img.shields.io/badge/Node.js-22.18%2B-5FA04E?logo=nodedotjs&amp;logoColor=white" alt="Node.js 22.18 or newer">
  <img src="https://img.shields.io/badge/TypeScript-3178C6?logo=typescript&amp;logoColor=white" alt="TypeScript">
  <img src="https://img.shields.io/badge/React-20232A?logo=react&amp;logoColor=61DAFB" alt="React">
</p>

**Understand every detail of an agent harness.**

Simplex is a lightweight C++20 harness for running LLM agents and learning how
an agent system works. Use the Hub’s browser panel to send a task, review tool
requests, and resume a saved conversation. Each session runs in its own worker.

[Documentation](https://hazer-bjtu.github.io/simplex-cpp/) ·
[Releases](https://github.com/Hazer-BJTU/simplex-cpp/releases/latest)

## Run your first session

For a local install, use Linux x86_64 with Bash, util-linux `flock`, and Node.js
22.18 or newer. On a host with glibc 2.34+, OpenSSL 3, and `ldd`, run:

```bash
npm install -g @hazer-bjtu/simplex-hub
simplex-hub install-worker --platform linux-amd64-standard --update-path
```

Choose `linux-amd64-bundled` to use the packaged runtime libraries instead.
The installer verifies the worker download and adds its `bin` directory to your
Bash PATH. Open a new Bash shell, then start the Hub with your provider key:

```bash
read -rsp 'Model API key: ' MODEL_API_KEY && echo
export MODEL_API_KEY
simplex-hub
```

1. Open the panel at `http://127.0.0.1:8800` (the default address).
2. In **Configurations**, create a worker configuration from the template. Set
   `providers` for your account, set `driver_model` to the provider entry’s
   name, and reference `${MODEL_API_KEY}` for its API key. Save the file.
3. Create a session with the `local` launch profile and your saved configuration.
   Select **Start**. When the worker is ready, send your first message.

## Security

Local tools can run commands and change files with the worker account’s
permissions. Run the worker under a restricted account or in a container when
it needs limited access. Load native plugins only from sources you trust.
