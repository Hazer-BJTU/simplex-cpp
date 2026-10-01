# Core documentation

The core package runs one agent worker per process. A worker initiates a
WebSocket connection to an external service, accepts user requests, and reports
agent-loop events. Separate one-shot connections carry tool-confirmation and optional remote-tool
requests.
The service may be implemented in any language or framework.

## Protocol reference

- [Worker client communication protocol](worker-protocol.md): connection setup,
  complete message formats, identifiers, event ordering, confirmation decisions,
  cancellation, failure handling, and recovery limits.

These documents describe implemented behavior. They are maintained alongside
protocol changes; proposed features are not presented as available operations.

## Publishing

This directory contains the source for the future documentation website.
Pages use ordinary Markdown, fenced examples, tables, and relative `.md` links
so they remain readable in the repository and can be processed by a static-site
builder for GitHub Pages. Keep page filenames and heading anchors stable. New
pages must be linked here. The documentation does not require C++ source access
to interpret a wire message.

No site generator or GitHub Pages deployment workflow is configured yet. When one is selected, its build must preserve or translate the relative
links and validate the published navigation.
