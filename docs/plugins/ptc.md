# Programmatic tool calling

<a id="experimental-python-tool-composition"></a>
<a id="model-interface"></a>

Programmatic Tool Calling (PTC) exposes one tool, `ptc_execute(source)`. The
model uses a Python program to combine installed functions and return selected
results. The optional interpreter runs on the worker target and accepts a
Python subset. Program-level tool approval applies before execution.

## Install and enable

Install the matching `simplex-ptc-vX.Y.Z-source.tar.gz` from
[GitHub Releases](https://github.com/Hazer-BJTU/simplex-cpp/releases) on the
worker target. Follow the included `INSTALL.md` to verify the archive and set
up the interpreter. [Installation requirements](https://github.com/Hazer-BJTU/simplex-cpp/blob/main/ptc/INSTALL.md)

Add `ptc` to the worker's tool selection and point `ptc.executable` to the
installed CLI. The following standalone-worker fragment selects PTC as its
only built-in toolset and enables the process and file functions:

```yaml
tools:
  intrinsic: [ptc]
persistence:
  enabled: true
  directory: ./data/session
ptc:
  executable: /opt/simplex-ptc/.venv/bin/simplex-ptc
  arguments: [--module, simplex_ptc.tools.process, --module, simplex_ptc.tools.files]
```

Replace the executable path with the one on your worker target. In a Hub-managed
worker template, keep the supplied session directory placeholder. Include other
toolsets in `tools.intrinsic` if the model should also have those tools.
Only enable trusted modules. Inside PTC, `manual()` lists the available functions.

## Session scope

PTC requires enabled persistence with an explicit directory. It stores its
interpreter state in `<persistence.directory>/ptc/`. Successful calls can reuse
state across worker restarts and conversation compaction. Clean-fork children
start with independent state. If `restore: never` is set, use a fresh directory
when starting a new interpreter session.

When configured, `worker.environment.workspace` is the interpreter's working
directory and must exist on the target. Commands and file edits can affect
resources available to the worker account. A failed program can leave external
effects even when its interpreter changes are discarded.
