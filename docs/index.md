---
layout: home
hero:
  name: Simplex
  text: Understand every detail of an agent harness.
  tagline: A lightweight native C++ worker, extensible agent loop, and a separate Hub for deployment and interaction.
  actions:
    - theme: brand
      text: Install and run
      link: /getting-started/installation
    - theme: alt
      text: Explore the architecture
      link: /architecture/overview
features:
  - title: One worker, one process
    details: Follow the path from a user request through model inference, tool execution, cancellation, and recovery.
  - title: Explicit extension boundaries
    details: Model providers, tools, and synchronous loop hooks share documented interfaces and lifetime rules.
  - title: Independent transport
    details: Build your own server against the Simplex Loop Worker Protocol or deploy the bundled Node.js Hub.
---

This site documents the repository's `main` branch. Published releases may lag
behind it. Use matching worker and Hub releases when deploying; capability
advertisements identify optional wire features.
