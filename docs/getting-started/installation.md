# Installation

Install the Hub on the machine that serves the browser panel. Install the worker
where it will run. For a local deployment, install both on the same Linux host.

## Requirements

The Hub requires Node.js 22.18 or newer and npm. The prebuilt workers target
Linux x86_64. Choose a platform tag for the installer:

| Platform | When to use it |
| --- | --- |
| `linux-amd64-standard` | The host has glibc 2.34 or newer, OpenSSL 3 libraries, and `ldd`. |
| `linux-amd64-bundled` | Use the bundled musl loader, OpenSSL, and runtime libraries. The launcher uses `dirname` and `readlink`. |

Both packages require Bash and util-linux `flock`.

## Install the Hub and worker

```sh
npm install -g @hazer-bjtu/simplex-hub
simplex-hub install-worker --platform linux-amd64-standard --update-path
```

Use `--platform linux-amd64-bundled` for the bundled package. The installer
downloads the latest stable worker from GitHub Releases, verifies its checksum,
and installs it under `~/.simplex/worker`. The `--update-path` option adds its
`bin` directory to your Bash PATH via `~/.bashrc`. Open a new Bash shell and verify:

```sh
simplex-hub --version
simplex --version
```

Use `--version vX.Y.Z` to select a stable release or `--directory DIR` to
choose the installation root. Before upgrading, stop workers that use this
installation. Upgrades replace the installation tree, so keep session data and
custom files elsewhere. See `simplex-hub install-worker --help` for other options.

## Manual worker download

Download your chosen archive and its release's `SHA256SUMS` into an empty
directory. Choose the filename ending in `-glibc2.34.tar.gz` for standard or
`-musl.tar.gz` for bundled. [GitHub Releases](https://github.com/Hazer-BJTU/simplex-cpp/releases/latest)

From that directory, set `archive` to the exact filename you downloaded:

```sh
archive=simplex-worker-vX.Y.Z-linux-x86_64-glibc2.34.tar.gz
sha256sum --check --ignore-missing SHA256SUMS && tar -xzf "$archive"
export PATH="$PWD/${archive%.tar.gz}/bin:$PATH"
simplex --version
```

Keep the extracted directory intact. For a source build, see
[Build locally](../building/local.md) or [Build with Docker](../building/docker.md).

## Start the Hub

Follow [Deploy the Hub and a worker](../deployment/hub.md) to configure a model,
start the service, and create your first session.
