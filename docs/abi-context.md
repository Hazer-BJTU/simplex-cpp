# Shared runtime and plugin ABI

Simplex plugins exchange live C++ objects with the host: JSON values, model and
tool instances, exceptions, and Boost.Asio coroutine frames. This is not a stable
C ABI or a sandbox. A domain ABI version is necessary, but it cannot make objects
built with incompatible compilers or runtime settings safe to exchange.

## Build host and modules in one context

Build the host and plugins together using the same compiler, C++ standard,
Boost headers and shared libraries, standard-library ABI, compile definitions
and project interfaces. The extension loader checks a generated toolchain
fingerprint and the domain's ABI version before construction. A passing gate is
an admission check, not a promise that arbitrary third-party binaries are safe.
Rebuild affected modules when any shared interface or ABI input changes. The
fingerprint check runs after `dlopen`, so module initialization and relocations
have already executed. It blocks incompatible factory calls, not untrusted code
execution during loading.

The installed worker archive contains runtime resources, not a standalone plugin
SDK. Start with the [plugin development workflow](plugins/development.md) and
the [local](building/local.md) or [Docker](building/docker.md) build guide.

## Keep shared state and type identity in shared libraries

Runtime implementation libraries are `SHARED`; dynamically loaded plugins are
`MODULE`. Do not statically embed independent copies of the project runtime,
Boost's compiled components, or libstdc++ into each plugin. Shared polymorphic
interfaces use out-of-line key functions/destructors where needed so their
vtable and typeinfo have an authoritative owner. Process-wide services such as
the synchronous event bus also live in a shared library rather than an inline
header-local singleton.

The host exports symbols (`ENABLE_EXPORTS`, implemented as `-rdynamic` on the
supported ELF toolchain); project targets are position independent. Provider
and adapter boundary tests check symbol ownership, exception identity and
object/library lifetime. Asio implementation types must not become substitute
public interfaces. Keep library handles alive until dependent objects and
coroutine work have finished; follow each domain's explicit ownership contract.

See the [generic extension framework](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/extensions/README.md)
and [model boundary documentation](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/llm/README.md)
for the fingerprint mechanism and structural tests. Tool and loop registries
have their own lifecycle rules; a shared runtime does not make mutable state
thread safe by itself.

## Install and portable releases

The installed worker and `simplex` launcher live in `bin/`; providers, toolsets
and hooks live under `bin/plugins/{llm,tools,loop}/`. Editable declarations live
under `bin/schemas/`, role and operation prompts under `bin/prompts/`, and the
startup template at `bin/config.example.yaml`. Project shared libraries live
under `lib/`. `$ORIGIN`-relative runtime paths let executables and modules find
the installed project libraries without a build-tree dependency.

A normal `cmake --install` does **not** copy every privately installed dependency.
For local builds, follow the [runtime library path instructions](building/local.md)
or arrange system library discovery. Official portable builds explicitly bundle
the selected GCC and Boost shared runtime using `SIMPLEX_BUNDLE_RUNTIME`; their
host requirements still include Linux x86-64, glibc and OpenSSL 3.

The portable image targets glibc 2.34. `docker/portability_floor.cmake` checks ELF
symbol-version requirements and the installed `DT_NEEDED` set. It limits OpenSSL
references to APIs available at the supported floor; identical OpenSSL SONAMEs
alone do not prove compatibility. Its narrowly named `_dl_find_object@GLIBC_2.35`
exception applies only to a bundled runtime whose symbol is backported on the
supported glibc-2.34 distributions. It is not a general relaxation for project
code. CI also executes the staged release on Ubuntu 22.04 and AlmaLinux 9.

These checks complement each other: fingerprint/domain checks protect admission,
symbol checks protect runtime composition and portability, and actual lifecycle
tests exercise construction, calls, cancellation and destruction. None turns a
plugin into an isolated process; load only trusted modules.
