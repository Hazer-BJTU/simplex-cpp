# Plugin roles

A worker assembles its plugins when it starts. Each worker session has its own
model instances, registered tools and loop hooks. Configuration determines
which capabilities the model can use and which observers run during a turn.

| Component | What it does | Scope |
| --- | --- | --- |
| Model provider | Connects a configured model to its endpoint. | The driver runs ordinary exchanges. An optional modality assistant handles isolated requests from its tool. |
| Toolset | Adds callable tools and any accompanying usage instructions to the model's context. | Selected tools act with the worker's permissions and tool approval policy. |
| Loop hook | Observes synchronous loop events. At designated edit points a hook can update conversation state. | Runs for that session. Changes recorded in conversation state can survive restoration. |

Providers are discovered as native modules. Toolsets and hooks can be built
into the worker or loaded as native extensions. For example, [PTC](ptc.md)
is a built-in toolset that calls a separately installed interpreter.

Set model roles, tool selection and extension names in the worker YAML. Settings
in shared installation files apply to new workers that read those files.
Native plugins execute inside the worker process. Use trusted plugin directories
and builds compatible with the worker. [Configure plugins](configuration.md) ·
[Security boundaries](../architecture/security.md#native-plugins)
