# Default worker prompt

`system_prompt.yaml` is the editable default for new worker sessions. CMake copies
it to `bin/prompts/system_prompt.yaml` and installs it in the same location
relative to the installation prefix. No prompt text is embedded in the worker.

Select a custom file through `worker.system_prompt_file` in the startup YAML.
Explicit relative paths use the startup file's parent directory. Omit the field
to load the default beside the executable. All files are validated at startup.

The format is an ordered `model_io::PromptTemplate`: an optional `heading_level`
and a required `sections` list. See [the format and lifecycle contract](../../load/README.md#system-prompt-files).
Use this file for base instructions; the worker separately injects the active
tool registry's skills. Names beginning with `skill.` are reserved.

Editing the file affects new sessions. Existing session snapshots keep their
stored prompt when restored; the worker does not hot-reload prompt files.
