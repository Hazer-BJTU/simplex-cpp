# Working in Simplex

These instructions apply throughout the repository. Obtain explicit approval
from the repository owner before adding, changing, moving, or removing any
`AGENTS.md` file. Keep this file focused on guidance that applies across modules.

## Start here

One C++ worker owns one session and runs its agent loop. The Node.js Hub
launches workers and serves the browser panel. The optional Python interpreter
for Programmatic Tool Calling (PTC) is a separate program.

Start with the README of the module that owns the behavior you are changing.
Find the entry point used by its callers and a test that exercises it. For a
change spanning several modules, the [architecture overview](docs/architecture/overview.md)
describes how they communicate.

## Change the owning component

Follow a public interface from its definition to its callers before changing it.
Two boundaries deserve particular care:

- **Worker protocol.** Define message shapes in `protocol/`. When a shape
  changes, check the worker and Hub producers and consumers, regenerate the
  committed manifest, and run the protocol drift tests. Keep lifecycle behavior
  in the component that implements it. See the [protocol guide](protocol/README.md).
- **Managed worker storage.** The Hub asks the launcher to invoke `simplex manage`
  on the worker target to inspect or remove an allocation. Preserve that path
  even when the Hub and worker share a host. The same operation must work when
  the files exist only inside a container or on a remote machine.

## Keep changes focused

Keep the patch within the agreed scope and preserve unrelated work in the
checkout. If a fix requires expanding the scope or changing a public contract
beyond what was agreed, explain the impact and confirm the change with the owner.

Write all repository documentation in English, including every `AGENTS.md`,
module README, and documentation page. When public behavior changes, update
the instructions or examples that a user would follow for that behavior.

## Verify the affected behavior

Start with the smallest test that can detect the change. When a change crosses
a process or package boundary, also exercise that boundary. For example, a
panel test with a stub Hub can verify rendering, while a change to worker
message delivery calls for a Hub test with a real worker.

Use the [contributing guide](CONTRIBUTING.md) for build and test commands and
the [CI guide](.github/ci/README.md) for integration coverage. Report which
checks ran, their results, and any relevant behavior that remains unverified.
