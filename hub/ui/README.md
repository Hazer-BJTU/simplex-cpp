# UI contracts

This directory owns the Hub's panel-facing wire types and static vocabulary.
Release builds expose it through `@hazer-bjtu/simplex-hub/ui`, with ESM constants
and TypeScript declarations. This entry point first becomes available in the
release containing issue #143's packaging change; older published versions do
not provide it. Source consumers can continue using `ui/index.ts` without a
prebuilt `dist/`. Existing `shared/protocol.ts` and `shared/configurations.ts`
imports are internal compatibility re-exports.

```ts
import { PANEL_PROTOCOL, PANEL_VERSION } from '@hazer-bjtu/simplex-hub/ui';
import type { PanelMessage, HubMessage, RestOperations }
    from '@hazer-bjtu/simplex-hub/ui';
```

Only `./ui` is a supported package API. There is no root server API, CommonJS
entry point, wildcard deep import or source-only export condition. The CLI bin
remains `simplex-hub`. A normal installation still installs the combined Hub
package's production dependencies; importing the SDK itself does not load or
require them. Consumers provide their own transport, validation and state.

## Ownership

- `protocol.ts`: WebSocket messages, protocol constants, session descriptions,
  confirmations, display history and existing REST response DTOs.
- `answers.ts`: exact-answer sources, queries and pages. Offsets and lengths
  count UTF-8 bytes; preserve source fingerprints and commit sequences unchanged.
- `configurations.ts`: operator configuration source, revisions, selections,
  template and endpoint-preview contracts. Preview endpoints are URL strings.
- `rest.ts`: all 26 panel REST method/path pairs, typed path parameters, query,
  request body and status-indexed responses; the static route and socket paths.
- `index.ts`: the deliberate export allow-list. Types use `export type`;
  runtime exports are static constants only.

Server, internal shared helpers and panel depend on these contracts. Contracts
never import back from those implementations. The SDK must not include clients,
parsers, guards, serializers, route builders, stores, retry/replay logic, React
components or executable initialization. Answer validators remain internal in
`shared/answers.ts`. Standard ECMAScript types are the only ambient dependencies.

## Reading the contracts

`RestOperations['POST /api/sessions/:id/answer']` describes its `params`, `query`,
`body`, status-indexed `responses` and common `error` envelope. `never` means
that the operation does not consume that part of a request. Query numbers must
be serialized into URL search parameters by the consumer. The keys already
contain the exact HTTP method and path; this is an inventory, not a router.

`RestResponse<Operation, Status>` includes the operation result and the common
error envelope. Status maps describe operation-specific replies, not an
exhaustive list of possible HTTP failures: authentication, validation, conflict
and server errors can occur on any authenticated route. Start/restart may return
`SupervisorResult` with `ok: false` at HTTP 409. Recovery can return a failed stop
result plus the session at HTTP 409. Force-kill refusals can be HTTP 200 with
`{ ok: false, error }`. Always inspect the body as well as the status. REST error
codes are open strings; the WebSocket `ERROR_CODES` array is not an exhaustive
REST list.

The local constants describe this package's vocabulary. They do not establish
what a remote Hub supports: inspect `/api/meta` or `welcome` capabilities.
Optional legacy fields remain optional. Tool arguments/results, worker event
`data`, persisted snapshot `state` and supervisor `config` remain opaque.
Types are not runtime validation: consumers must validate untrusted replies.
Configuration source may contain secrets; never copy it into logs. Wire paths
and process IDs are diagnostics, not filesystem or process-control authority.

`/api/meta` is public. Other panel REST routes require panel authentication.
The panel WebSocket uses `/panel/ws`, token authentication and the current
Origin policy. These contracts introduce no cross-origin permission or new
worker operations. History/input/signals/confirmation remain WebSocket messages.
See [Hub protocol](../../docs/hub/hub-protocol.md),
[configuration library](../../docs/hub/configurations.md) and
[subagents](../../docs/hub/subagents.md) for authoritative behavior.

## Changing a wire field or route

1. Edit its canonical declaration here once, preserving optional and open fields
   unless a protocol compatibility decision explicitly changes them.
2. Add new public names to the explicit barrel. Keep private launcher, token,
   persistence and process models in their implementation modules.
3. Update the trusted response construction and existing input validation in the
   handler. Do not cast untrusted input to a public request type to bypass checks.
4. Add the REST contract and descriptor together when adding a route. Update the
   protocol documentation and representative reply/negative compile fixtures.
5. Run `npm run check:ui`, `npm run typecheck`, `npm test`, and `npm run build`.

`check:ui` compiles the contracts and negative fixtures with `lib: ["ES2023"]`,
`types: []`, strict checking and declaration checking enabled. Its structural
boundary check traverses runtime and type-import dependencies, scans unexported
modules too, and rejects functions/classes, external or backwards imports,
ambient references and executable initializers. The parser is a development
check dependency only. Tests compare live route declarations to both the static
inventory and typed operation table, check actual Hub replies and preserve the
existing protocol drift tests.

`build:release` separately emits `dist/ui/*.js` and `*.d.ts`, then rewrites the
declarations' relative `.ts` specifiers to `.js` so they resolve to installed
sibling declarations. Package checks inspect both graphs and run NodeNext and
Bundler consumers outside the checkout with no Node/DOM ambient types. A bare
archive fixture imports constants without any installed dependencies; an ESM
smoke blocks network, filesystem writes and timer/listener side effects. A
browser build checks that its graph stays inside the SDK without Node polyfills.

The same checks run from the existing CI and release package validator. It
creates a temporary toolchain from the repository lockfile (including development
dependencies, with install scripts disabled), even on production-only publish
runners. Tools and consumer fixtures are separate directories; neither source
aliases nor repository `node_modules` can supply consumer imports. Cold checks
may download locked tooling; all temporary files are removed afterwards. The
package adds no runtime tools and does not mark the entire Hub side-effect-free.
Bundled panel migration and complete external examples remain PR 3 of #143.
