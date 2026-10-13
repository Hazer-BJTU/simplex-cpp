# Bundled runtime notices

These notices accompany the dynamically linked musl, GCC runtime, OpenSSL and
Mozilla CA certificate data in the musl worker package. Repository-maintained
Boost, JSON and yaml-cpp notices are added by the archive packager separately.

Sources used for the initial toolchain (see runtime-packages.txt for actual
Alpine package versions):

- musl 1.2.6 `COPYRIGHT`: https://distfiles.alpinelinux.org/distfiles/v3.24/musl-1.2.6.tar.gz
- OpenSSL 3.5.9: https://github.com/openssl/openssl/blob/openssl-3.5.9/LICENSE.txt
- GCC 15.2.0 GPLv3 and Runtime Library Exception: https://github.com/gcc-mirror/gcc/tree/releases/gcc-15.2.0
- Mozilla CA data, MPL 2.0: https://www.mozilla.org/MPL/2.0/ (text from https://github.com/spdx/license-list-data/blob/v3.27.0/text/MPL-2.0.txt).

The CA bundle is generated from Mozilla's trust data through Alpine's
ca-certificates package. Upstream sources: https://hg.mozilla.org/projects/nss/
and https://gitlab.alpinelinux.org/alpine/ca-certificates.

When updating runtime packages, review these notices and retain the applicable
runtime-library exception. This package does not include the compiler itself.
