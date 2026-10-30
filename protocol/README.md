# protocol

The worker's wire vocabulary: one explicit description per record, the codec
that reads and writes it, and the generated contract document that consumers can
inspect without building C++.

This package is the authority for *shape*: field names, presence, nullability,
closed vocabularies and which record an event carries. Behaviour — ordering,
backpressure, cancellation, confirmation deadlines, authorization, persistence
atomicity — stays in its owning modules and in `docs/core/worker-protocol.md`.

## Layout

| Path | What it is |
|---|---|
| `include/protocol/descriptor.hpp` | Field, enum and record descriptors; one `wire_fields()` per record |
| `include/protocol/codec.hpp` | `to_json` / `from_json` driven by those descriptors |
| `include/protocol/wire.hpp` | The inventory: envelopes, requests, events, shared objects, confirmation, remote tools |
| `include/protocol/manifest.hpp` | The published document, its JSON Schema view and the fingerprint |
| `src/wire.cpp` | Cross-field rules and the event registry |
| `src/manifest.cpp` | Registry walk, canonical document, SHA-256 |
| `tools/export_manifest.cpp` | The only supported producer of `schemas/worker-protocol.json` |
| `schemas/worker-protocol.json` | Generated and committed; never edited by hand |
| `test/` | Codec, registry and staleness regressions |

## The three artifacts

`protocol_manifest_export` prints one of them:

```sh
protocol_manifest_export            # published manifest
protocol_manifest_export --hash     # fingerprint only
protocol_manifest_export --canonical
protocol_manifest_export --schema   # JSON Schema 2020-12
```

The **canonical contract** is structure only. Explanations are excluded from it
on purpose, so editing a sentence cannot move the fingerprint; a changed field,
presence rule or enum spelling always does.

The **fingerprint** is lowercase hex SHA-256 of that canonical document. It
certifies the structural contract and nothing else — `fingerprint_scope` in the
manifest lists exactly what remains prose. A matching fingerprint is not a
compatibility promise and never gates a connection.

## Updating the committed artifact

`schemas/worker-protocol.json` is a review and consumer artifact, not a source:

```sh
cmake --build build --target protocol_manifest_export
build/bin/protocol_manifest_export > protocol/schemas/worker-protocol.json
```

`test_protocol_manifest` fails when the file is stale and prints that command.
The check is native-only by construction: the generator is a normal executable
built from the same sources, so there is no generation cycle and no target
binary is executed during a cross build. A cross build defers the freshness
check to staged native validation.

## Design decisions

**Presence and nullability are separate.** A `std::optional<M>` member defaults
to an optional wire field; other members default to required. Descriptor
overrides express required-but-nullable fields such as persistence revisions.
The codec and manifest read the same effective rules.

**Open points stay open.** `extras`, tool arguments, provider option values and
route results are `opaque` (`nlohmann::json`). The manifest describes their
policy — provider-, tool- or plugin-owned — instead of pretending to constrain
them. `manifest()` lists those policies in `extension_policies`.

**Conditional requirements are shared descriptors.** `PayloadRequest` is one flat
wire object whose required fields depend on `operation`; `wire_rules()` is
where that lives, so `history` carrying `content` fails rather than being
half-read.

**Remote routes belong to the Hub.** The protocol describes correlation fields,
request/response discriminators, the success/rejection envelope and opaque
argument/result objects. It has no route registry or per-tool argument schemas.
The Hub decides route availability, authorization and argument validity. A
concrete worker tool may validate its own model-facing arguments and the fields
it needs in a reply; those checks stay with that tool. Unknown Hub error codes
remain readable diagnostic strings. Neither a route addition nor a tool schema
change changes the worker protocol fingerprint.

**Runtime policy stays in the application.** Payload `options` is an opaque
object: category names, confirmation modes and provider-specific settings are
not a closed protocol vocabulary. The codec preserves them for application
validation, which reports unsupported values as business errors. The protocol
keeps request field types and basic operation combinations, but does not impose
ID/text length limits, page-size quotas or application-owned resource defaults.
Unsigned counts/cursors accept the full range 0 through 18446744073709551615
(UINT64_MAX); this representable range is part of the fingerprint and Schema.
Status capabilities are an extensible string list: unknown names round-trip,
while the `Capability` enumeration documents known names for producers. Status diagnostics
remain described in detail. Application parsing retains its runtime safeguards separately from structural decoding.

**Two libraries.** `protocol_lib` (`libsimplex_protocol.so`) is the descriptors
and codec and depends on nlohmann/json only — a tool or plugin can speak the wire
format without acquiring a crypto dependency. `protocol_manifest_lib` adds the
manifest and the SHA-256, using OpenSSL::Crypto, which is already the project's
hashing dependency behind `openssl_iface`. `display.cpp` hashes answer sources
the same way.

**The worker now builds its messages here.** `core` links `protocol_lib` and
constructs event envelopes, metadata events, projections, confirmation requests
and remote-tool envelopes from these records; `core/wire.hpp` owns the enum
conversions. Behaviour stays where it was: event classification, budgets,
sequence allocation and admission policy remain application code, and the
projection algorithms keep their byte accounting. `simplex protocol` prints the
compiled manifest; `simplex protocol --hash` prints its structural SHA-256.
Direct equivalents are `simplex_worker --protocol` and `--protocol-hash`.
Neither command initializes the runtime or reads external schema files.

## Relationship to the documentation

`docs/core/worker-protocol.md` remains the behavioural guide. The compiled
manifest is authoritative for field types and presence; documentation examples
explain use and do not drive compatibility checks. The reference artifact is
installed at `share/simplex/protocol/worker-protocol.json` and copied verbatim
into the Hub npm package at `dist/protocol/worker-protocol.json`.

Hub Node tests validate adapter output using the committed artifact and Ajv's
Draft 2020-12 validator with the trailing-marker extension below. Required-field,
enum and type mutations verify that mismatches fail. Worker-enabled e2e compares
the actual binary's output against that same artifact. Runtime connections do
not load this validator and unknown-event tolerance remains unchanged.

When changing a structural descriptor, build `protocol_manifest_export`, run
`build/bin/protocol_manifest_export > protocol/schemas/worker-protocol.json`,
then run protocol native tests and `cd hub && node --test test/protocol-drift.test.js`.
Do not edit the JSON artifact manually. Cross-compilation does not execute a
target generator during configure/install: use a host build to update the
committed artifact, and compare the target CLI in a compatible runtime image.

## Structural conformance tests

Operation-specific required, forbidden, ignored and closed-field policies live
in `Rule`. Both the decoder and schema generator use that table; the canonical
contract includes every policy, so changing field acceptance changes the hash.
Fixed display unions are described separately from tool-owned opaque JSON.

Run the independent schema tests in a Python virtual environment:

```sh
python -m pip install -r protocol/test/requirements.txt
python protocol/test/test_schema.py
```

CI runs these checks against the committed schema, and the native tests verify
that the compiled exporter produces that exact artifact. Draft 2020-12 checks
the projected batch item shapes and permits at most one omission marker. Since
it cannot express the final position of an optional marker in an unbounded
array, `x-simplex-trailing-marker` additionally specifies that ordering rule.
The independent validator implements this keyword; generic validators that
ignore extensions validate only the standard-schema subset. The batch kind
and both record references are fingerprinted, and the C++ codec enforces the
same ordering directly from that field descriptor.
