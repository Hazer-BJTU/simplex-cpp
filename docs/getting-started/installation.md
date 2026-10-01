# Installation

The quickest deployment runs the worker and Hub on the same Linux machine.
The worker release and Hub npm package are installed separately.

## Requirements

| Component | Requirement |
| --- | --- |
| Prebuilt worker | Linux x86_64, glibc 2.34 or newer, host OpenSSL 3 libraries, Bash |
| Hub | Node.js 22.18 or newer and npm |
| Model | Credentials and an endpoint supported by an installed provider |

The archive includes C++ runtime libraries, plugins, schemas, and prompts.
It is not a statically linked binary. Keep its directory structure intact.

## Download the worker

Download the worker archive and `SHA256SUMS` from
[GitHub Releases](https://github.com/Hazer-BJTU/simplex-cpp/releases/latest).
Run these commands in the download directory containing that release's files:

```sh
sha256sum -c SHA256SUMS
tar -xzf simplex-worker-v*-linux-x86_64-glibc2.34.tar.gz
export PATH="/absolute/path/to/extracted-worker/bin:$PATH"
simplex run --help
```

Replace the example path with the extracted directory. A checksum detects file
corruption; it does not independently authenticate the publisher. Add the PATH
export to your shell startup file if desired.

## Install the Hub

```sh
npm install -g @hazer-bjtu/simplex-hub
simplex-hub --help
```

The package includes the compiled server and browser panel; no separate panel
build is required. The Hub's default local launcher expects `simplex` on PATH.

## Start the Hub

```sh
export MODEL_API_KEY='your-api-key'
simplex-hub --data-dir "$HOME/.simplex/hub"
```

Open `http://127.0.0.1:8800`. The Hub creates its configuration library on first
startup. Continue with [configuration](configuration.md) before starting a
worker. Set credentials before starting the Hub so local workers inherit them.

For an installation built from source, see [local builds](../building/local.md)
or [the published build images](../building/docker.md). To deploy away from the
Hub host, see [Docker](../deployment/docker-worker.md) or
[remote workers](../deployment/remote-worker.md).
