# Role-based worker prompts

Each YAML filename identifies a role. `coding_agent.yaml` provides the concise,
general-purpose coding agent and is the default for new worker sessions. CMake copies
it to `bin/prompts/coding_agent.yaml` and installs it in the same location
relative to the installation prefix. No prompt text is embedded in the worker.

Select a custom file through `worker.system_prompt_file` in the startup YAML.
Explicit relative paths use the startup file's parent directory. Omit the field
to load the default beside the executable. All files are validated at startup.

The format is an ordered `model_io::PromptTemplate`: an optional `heading_level`
and a required `sections` list. See [the format and lifecycle contract](../../load/README.md#system-prompt-files).
Use these files for base instructions; the worker separately injects the active
tool registry's skills. Names beginning with `skill.` and the names `environment.runtime` / `signature.runtime` are reserved.
The worker refreshes configured environment hints after tool skills at startup,
including on restore; see [environment configuration](../../load/README.md#runtime-environment-hints).

Editing the file affects new sessions. Existing session snapshots keep their
stored prompt when restored; the worker does not hot-reload prompt files.

The worker appends a short, untitled `signature.runtime` Volatile section after
all other sections: a simplex version welcome, a greeting to the configured
provider, and a wish for successful tasks. This decorative footer is regenerated
at startup, including on restore, using the current build version and provider.
It is host-owned and cannot be declared in a role YAML file.
