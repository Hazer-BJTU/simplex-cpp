# Simplex worker binary release

This archive contains the Linux x86_64 worker, its built-in and extension
plugins, configuration templates, prompts, and bundled C++ runtime libraries.
It targets glibc 2.34 or newer and uses the host's OpenSSL 3 libraries.
Keep the directory together: executables locate plugins and libraries relative
to their own paths.

Extract the archive, then run `bin/simplex run --help` from its root. Create a
worker configuration from `bin/config.example.yaml` and start it with
`bin/simplex run --config /path/to/config.yaml`. The worker connects to a Hub;
the Hub and its web panel are separate components and are not in this archive.

Check the downloaded file with `sha256sum -c SHA256SUMS` in the directory that
contains both the archive and the checksum file. This detects corruption; it
does not authenticate the publisher. See the repository's release page for the
source tag and build workflow.

The top-level `LICENSE` and `third_party_licenses/` contain the license notices
maintained in this repository.
