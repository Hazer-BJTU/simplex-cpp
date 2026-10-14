# Read-only UI SDK examples

These examples are not SDK exports and are not a production client library.
`observer.mjs` owns the examples' limited validation and replay handling;
`tui/main.mjs` owns terminal output, while `web/main.mjs` owns browser controls
and uses textContent only. Reconnect is explicit and starts from zero. Only
ordinary-session discovery, correlated paged transcript replay and live event
labels are implemented. The last 200 retained event labels are shown; omissions
are explicitly marked. No user input, approval or lifecycle command is sent.

Install the actual packed Hub archive here (or in a copy of this directory):

```bash
npm install --ignore-scripts /absolute/path/to/hazer-bjtu-simplex-hub-VERSION.tgz
SIMPLEX_PANEL_TOKEN=... node tui/main.mjs http://127.0.0.1:8787 SESSION_ID
```

Prefer reading the token from a secret prompt/environment rather than typing it
into shell history. Omitting SESSION_ID lists ordinary sessions. The browser
example needs the same-origin reverse proxy and local Vite build described in
[the SDK guide](https://hazer-bjtu.github.io/simplex-cpp/hub/ui-sdk.html).
It does not implement general cross-origin deployment, automatic reconnect,
approvals, worker history or full answer retrieval. Both `transcript-pages` and
`transcript-epoch` are required; unknown capabilities are not guessed.

The package validator copies these examples outside the repository, installs
the packed SDK, builds the Web entry and tests both adapters against a real Hub.
No source aliases, private package imports or changes to the SDK boundary are
needed to run them. Package-relative imports in the examples use only `./ui`.
