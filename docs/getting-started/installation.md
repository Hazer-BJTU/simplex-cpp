# Installation

The quickest deployment runs the worker and Hub on the same Linux machine.
The worker release and Hub npm package are installed separately.

## Requirements

| Component | Requirement |
| --- | --- |
| Prebuilt worker | Linux x86_64, glibc 2.34 or newer, host OpenSSL 3 libraries, Bash |
| Hub | Node.js 22.18 or newer and npm |
| Hub worker installer | `flock` (util-linux) and `ldd` on the worker host |
| Model | Credentials and an endpoint supported by an installed provider |

The archive includes C++ runtime libraries, plugins, schemas, and prompts.
It is not a statically linked binary. Keep its directory structure intact.

## Install the Hub and worker

```sh
npm install -g @hazer-bjtu/simplex-hub
simplex-hub install-worker --update-path
```

The standalone installation command downloads the latest stable worker from the
official GitHub Releases, verifies `SHA256SUMS`, checks the extracted tree and
runtime libraries, and runs a bounded `simplex run --help` startup check. It
does not start the Hub, load Hub configuration, or require model credentials.
GitHub is the default and currently the only supported source.

The default installation root is `~/.simplex/worker`; choose a different directory
or a specific stable release with:

```sh
simplex-hub install-worker --source github --version v0.2.0 \
  --directory "$HOME/apps/simplex-worker" --no-update-path
simplex-hub install-worker --help
```

The chosen directory contains `bin/`, `lib/`, and the other release resources
directly, without a version-named wrapper directory. Keep this structure intact.
The host must provide `libssl.so.3` and `libcrypto.so.3`; Node's bundled OpenSSL
does not satisfy that requirement. Startup validation clears development
`LD_LIBRARY_PATH`, `LD_PRELOAD`, and `LD_AUDIT` variables.

### Replacement and versions

| Option / behavior | Meaning |
| --- | --- |
| Same installed version | Checks required files and executable permissions, then skips downloading/replacing; still honors PATH updates |
| Newer version | Replaces a recognized installer-managed installation |
| `--reinstall` | Replaces the current version, for example to repair missing files |
| `--allow-downgrade` | Authorizes installing an older known version |
| `--overwrite` | Authorizes replacement of an unknown non-empty directory; does not authorize downgrades or dangerous targets |

Stop workers using this installation before replacing it. The installer does
not supervise or stop running workers. Complete-tree replacement removes custom
and stale files inside the installation root; keep session state, archives,
custom prompts, and other data outside it. Root, home, protected system
directories, and symlinked destinations are rejected even with `--overwrite`.

Preparation happens separately from the live directory. Failed downloads,
checksums, extraction, or startup checks leave the previous installation intact.
Replacement uses a rollback record and an adjacent private
`.simplex-install-<hash>/` workspace, with a persistent kernel-lock file. If the
process is interrupted during replacement, the next installation into the same
directory recovers before proceeding. Do not remove recovery files while an
installation is active or a rollback needs recovery. Concurrent installation
attempts fail clearly instead of replacing each other's files.

### Optional Bash PATH update

`--update-path` writes one marked block to the current user's `~/.bashrc`.
Later installations replace that block, including when the installation directory
changes. Unrelated shell configuration and existing file permissions are
preserved. Complete duplicate blocks are consolidated; malformed markers or a
symlinked `.bashrc` require manual correction instead of an ambiguous rewrite.
PATH editing has its own lock, independent of the worker destination.

`--no-update-path` leaves `.bashrc` unchanged. Without either flag, an interactive
terminal asks once; non-interactive use skips the update and prints a safely
quoted manual PATH command. EOF or cancellation declines the prompt. A PATH
write failure reports that the worker was installed and returns a nonzero exit
code; it does not undo the worker installation.

Open a new Bash shell or run `source ~/.bashrc`, then restart the Hub so it
inherits the updated PATH. Existing shells and already-running Hubs retain their
environment. Other shells require manual PATH configuration. Without changing
PATH, put the absolute `<installation>/bin/simplex` path in `launcher.command[0]`
of your saved local launch configuration.

Downloads install executable code and dynamic plugins. Checksums detect
corruption; a checksum downloaded from the same release does not independently
authenticate the publisher.

## Manual worker download

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
