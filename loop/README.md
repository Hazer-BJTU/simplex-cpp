# loop

A harness process may have only one active `loop::run()` call, including synchronous hook execution and tool cleanup. The caller enforces this rule. This package has no gate, queue, global run instance, or separate journal. Within a batch, the registry may still execute tools in parallel when their declarations allow it.

The package implements the normal agent loop, synchronous writable hooks, result commits and recovery, and interruption of model waits through `std::stop_token`. The [interactive example](example/README.md) uses it with DeepSeek and the process toolset. The older example remains available.

Hooks can be linked directly from [`intrinsic/`](intrinsic/) or loaded from
[`extensions/`](extensions/). Both use `LoopHookInterface` and the same session
registry; see the dynamic package's developer guide for factory signatures,
YAML layout, and plugin discovery.

## Usage

Link `loop_lib` and include `loop/loop.hpp` and, when subscribing to events, `loop/events.hpp`:

```cpp
model_io::AgentInputState state;
std::stop_source stop;
model_io::MessageItem input;
input.type = model_io::MessageItemType::UserInput;
input.role = "user";
// Fill input.content; configure state.system_prompt and state.tools according to host policy.
auto result = co_await loop::run(
    model, registry, bus, executor, state,
    true, std::move(input), {.max_exchanges = 32}, stop.get_token());
```

All dependencies are injected explicitly. The loop does not load a model, register tools, inject skills, or read a global bus. Referenced dependencies and `state` must remain alive until `run()` returns; the input and options enter the lazy coroutine frame by value. The host owns the model, registry, and bus lifetimes.

With `has_message = false`, the message argument is ignored (you may pass `{}`). The loop continues the last turn without creating user input or rerunning existing tool calls. It returns `Failed` if there is no turn. `max_exchanges` must be greater than zero; each model response counts as one exchange. If the final allowed response contains tool calls, the loop fully settles that batch before reporting `ExchangeLimit`.

## State and recovery

`AgentInputState` is the only persistable state. Its optional `loop` field records the current status, phase, exchange count, error, and `pending_results` when result projection fails. This field is host metadata, not a provider request parameter. Commit sequences changed the C++ layout, so the model plugin ABI is now version 6; older plugins must be rebuilt. Older JSON without these fields remains readable. `RunResult` only summarizes the current invocation and does not need separate persistence.

Each committed model response increments `LoopProgress::committed_response_sequence` and writes the sequence to its `AgentLoopStep`. The sequence survives across `run()` calls and never decreases when old steps are pruned; older JSON without a sequence reads it as zero. A hook that maintains cumulative statistics can store both its last accounted sequence and its totals in `external_status`. It can then reconcile an edit rollback and detect gaps if unaccounted responses were pruned.

The normal flow is input commit → model request → response commit → registry batch → result commit → next request. A model response without tool calls completes the run.

Return statuses are `Completed`, `Cancelled`, `ExchangeLimit`, and `Failed`. Diagnostics use logging; tool output and error records go into the dataclass. Logs are not a recovery source. On a normal exit, the loop stores `loop.status` and `loop.error`, runs the writable `EditOnRunFinished` hook, and synchronously publishes the read-only `RunFinished` event. Invalid parameters or a stop requested before admission do not publish run events or overwrite previous run information.

Every `model.integrate()` operates on a candidate state and commits with nonthrowing move assignment only after success. Complete tool returns first move into `state.loop.pending_results`, then project together into a candidate conversation. A projection failure cannot leave half-written tool messages: the next `run()` projects those results before accepting new input or requesting a model response. This buffer contains only uncommitted results; it is cleared after a successful projection and is not a second copy of the execution history.

`LoopProgress::status` uses `model_io::LoopStatus` (`Idle`, `Running`, `Completed`, `Cancelled`, `ExchangeLimit`, `Failed`); `phase` uses `model_io::LoopPhase` (`Ready`, `Model`, `Tools`, `Projection`, `Blocked`). JSON retains the lowercase names, such as `exchange_limit` and `projection`. Deserialization rejects unknown names or incorrect types. On recovery, `Tools` or `Blocked` means tool results are uncertain: `run()` throws `loop::RecoveryRequired` and leaves investigation and repair to the host. It neither replays nor resolves the tools automatically. `Projection` must have pending results; continuation requires validating the count, order, IDs, and names of every tool call and result. Imported unanswered calls are also ineligible for automatic replay. Callers must not tamper with history or recovery markers.

The host can catch `RecoveryRequired`, inspect `phase()` to distinguish `Tools` from `Blocked`, and compare persisted state with external tool effects. It must not treat this exception as an automatically retryable model failure. Currently `LoopProgress` lives alongside the conversation in the sole `AgentInputState`. A future design could move host recovery metadata into another persistable dataclass and expose only conversation data at the model boundary, avoiding model plugin ABI changes whenever the recovery protocol changes. This implementation retains the current state contract.

This is a recovery protocol for a live process, not a write-ahead log. The host decides when to persist the dataclass. The loop cannot promise recovery of effects after arbitrary crashes, power loss, or memory exhaustion, nor infer external details that tools did not report.

## Exception handling

`run()` converts ordinary runtime exceptions into `RunResult`; callers normally inspect `result.status` and `result.error`. A previous `Tools` or `Blocked` phase is different: admission is rejected with `RecoveryRequired` carrying the original phase, leaving state unchanged for the host to handle. Exception handling has three layers:

1. `converse_interruptibly()` waits for the model coroutine to exit. On an exception it closes this invocation's cancellation bridge and rethrows unchanged; it does not classify the failure.
2. The model-wait boundary recognizes cancellation only when both `stop.stop_requested()` is true and a `boost::system::system_error` has error code `operation_aborted`. Neither condition alone proves cancellation.
3. The run body rethrows `RecoveryRequired` separately, then catches all other exceptions with `catch (...)`, including failures in parameter validation, projection recovery, model calls, history integration, registry dispatch, and synchronous hooks. It records a log and returns `Failed`. Diagnostics use `what()` for `std::exception` and `unknown exception` otherwise.

The built-in Chat Completions and Responses adapters call `endpoint::complete`. After cancellation reaches transport, an HTTP error in flight can also become `operation_aborted`. The exception that actually completes determines the outcome when an HTTP failure races with stop: an independent failure completed first yields `Failed`, while terminal cancellation propagated into transport and completed as `operation_aborted` yields `Cancelled`. A nonretryable HTTP error or exhausted retries that throw `HttpRequestException` unchanged reach the third layer. The loop does not retry model requests itself. Errors returned as tool values remain tool results and do not automatically fail the whole loop; exceptions escaping the registry do.

Failure state depends on where the error occurs:

| Boundary | State and next action |
|---|---|
| Ordinary validation or projection recovery before admission | Return `Failed`; create no new progress record and publish no `RunFinished`. Previously recovered results may already be committed. |
| Previous phase is `Tools` or `Blocked` | Throw `RecoveryRequired` with the original phase; do not replay tools or admit this run. |
| Model request | Do not commit a partial response; change `Model` back to `Ready`. |
| Tool phase fails before complete results are saved | Change `Tools` to `Blocked`; forbid automatic replay. External effects already made are not rolled back. |
| Tool-result projection | Retain `Projection` and the complete `pending_results`; retry projection only. |
| Synchronous hook | Preserve committed data as described below; when needed, first close declared calls with skipped-tool results, then finish the run. |
| `EditOnRunFinished` | Roll back this state edit, set `Failed`, append a diagnostic, and still publish `RunFinished`. |
| `RunFinished` | Terminal state is already recorded. A subscriber exception is logged only; it changes neither the return value nor persisted state, and does not republish the event. |

Once a run is admitted, the loop saves the terminal status and error before publishing `RunFinished`. `Failed` means this invocation failed; it does not mean the whole input, conversation history, or external tool effects were rolled back.

`run()` is **not a `noexcept` boundary**. Exceptions from argument or coroutine-frame construction, coroutine initialization, and secondary failures while creating diagnostics, saving terminal state, or logging can still reach the caller. The host should retain an outer exception handler while awaiting `run()` and must not treat an abnormal return as permission to replay tools.

## Synchronous hooks

To organize event handlers as stateful plugins, implement the public top-level [`LoopHookInterface`](include/loop/hook_interface.hpp) and hand instances to the session-level [`LoopHookRegistry`](include/loop/hook_registry.hpp). The registry uses the same synchronous bus passed to `run()` and manages instance and subscription lifetimes by plugin name: `add()` registers, `set()` adds or replaces, `get()` looks up, and `remove()` and `clear()` unsubscribe. A plugin's `subscribe()` must return all subscription handles. The underlying `LoopHookBinding` owns both the plugin and its handles and disconnects listeners before destroying the plugin. Bind and unbind serially with `run()`; the bus must outlive the registry.

Give each connection returned by `bus.subscribe()` to a local `ScopedSubscription` before moving it into the subscription container: growing that container can throw. Passing a raw connection directly to `emplace_back()` can leave a callback pointing to a destroyed plugin if allocation fails. Replacing a plugin with `set()` puts its new callbacks after existing callbacks. If ordering matters, remove dependent later plugins between runs, replace the first one, then register the dependents again.

```cpp
eventbus::EventBus bus;
loop::LoopHookRegistry hooks(bus);
hooks.add(std::make_shared<MyLoopHook>());
// Keep bus and hooks alive throughout every loop::run() call.
```

Plugin instances may hold state within the process. Data needed across process restarts still belongs in `AgentInputState`. Built-in plugins live under [`intrinsic/`](intrinsic/) and are not declared through `extensions`. Their YAML configuration is read when constructing an instance and installed by `cmake --install` under `bin/schemas/loop/<plugin-name>/config.yaml`. See that directory's README for configuration format, path overrides, and error handling. The built-in [`ContextStatisticHook`](intrinsic/hooks/context_statistic/README.md) writes per-exchange token statistics to `external_status.context_statistic` for other hooks to read.

The bus is an explicitly injected synchronous `EventBus`. Callbacks run in subscription order, and the loop resumes only after they return. Event references are valid only during the callback; do not retain them, mutate the current state through an external alias, or reenter `run()`.

| Event | Access and timing |
|---|---|
| `RunStarted` | Read-only; after admission. |
| `BeforeInput` | Writable candidate user message; must remain `UserInput` without tool calls or returns. |
| `InputCommitted` | Read-only; user message committed. |
| `BeforeModel` | Writable candidate `system_prompt`, `tools`, and `extras`; history and recovery metadata are read-only. |
| `ModelCommitted` | Read-only; model response committed. |
| `BeforeToolBatch` | Read-only call list; tools have not started. The host may request stop through its `stop_source`. |
| `ToolDispatchCheckpoint` | Read-only state after phase becomes Tools, before dispatch. A throw prevents dispatch and leaves Blocked for conservative inspection. |
| `ToolResultsCheckpoint` | Read-only state with the complete batch buffered in Projection. A throw preserves the buffer for recovery without replay. |
| `ToolResultsCommitted` | Read-only; all results, including skipped calls, are written to the conversation. |
| `EditOnStepFinished` | Edits the full state in place after the preceding event succeeds and before the next model request. |
| `StepFinished` | Read-only state after the step edit transaction passes validation. A throw fails the run without rolling back validated edits. |
| `EditOnRunFinished` | Edits the full state in place after terminal state is saved and before `RunFinished`. |
| `RunFinished` | Read-only; terminal state saved. |

Multiple `BeforeInput` or `BeforeModel` subscribers edit the same candidate in order; if any subscriber throws, that stage's changes are not committed. Before committing model context, the loop verifies that the prompt can be rendered. The host is responsible for catalog contents, provider parameter semantics, and consistency with the registry; the loop does not rewrite these by policy.

For example:

```cpp
auto subscription = bus.subscribe<loop::BeforeModel>(
    [](const loop::BeforeModel& event) {
        event.context.extras = nlohmann::json{{"custom_parameter", 42}};
    });
```

The synchronous constraint applies to loop hooks. Existing tool authorization in the registry keeps its own asynchronous mechanism, and the loop waits for the entire tool execution path.

Events themselves are const; object references declared writable in them permit synchronous mutation. To update plugin state, use the established `external_status` or `events` areas of candidate `extras` according to the dataclass contract. Do not modify candidate objects asynchronously from callbacks.

A hook exception ends the current run. If a hook fails after a model response commits but before tool execution, the loop adds skipped results for declared calls, closing their call/result relationships. Results of tools already executed are not rolled back by a later hook failure.

If an `EventBus` subscriber throws, later subscribers for that event do not run. `EditOnRunFinished` can alter terminal state, with its state edit rolled back on failure; `RunFinished` then only announces the final outcome. If a `RunFinished` subscriber throws, later subscribers do not run, the exception is logged, and earlier observers and the `run()` caller see the same terminal state.

## Full-state editing and pruning

The `state` references in `EditOnStepFinished` and `EditOnRunFinished` both refer to the caller's original `AgentInputState&`. Subscribers edit it directly in order, so later subscribers see earlier edits. The reference is valid only during the callback: do not retain it, access it asynchronously, observe intermediate state from another thread, or reenter `run()`. Serialize subscription changes with loop execution; a subscriber added after the no-subscriber check may not participate at the current boundary.

`EditOnStepFinished` runs once per settled tool batch after all `ToolResultsCommitted` subscribers return successfully. This includes batches with skipped results due to stop and the final batch at the exchange limit. It is not published for a response without calls, recovery of old results on entry, or failure of an earlier observation hook. Uses include pruning tool history, summarizing older turns, and preparing the next model context. The edited state is used by the next model request.

`EditOnRunFinished` runs once per admitted invocation after terminal state is saved, covering `Completed`, `Cancelled`, `ExchangeLimit`, and `Failed`. It can organize final history, update host summaries, or adjust persistence metadata. Its read-only `result` describes the outcome on entry to the hook. Rejected parameter errors, stop before admission, and failed entry recovery do not publish it. If this hook fails, the loop rolls back its edits, appends a diagnostic prefixed `EditOnRunFinished:`, changes the outcome to `Failed`, and still publishes one `RunFinished`. Final persistence therefore usually belongs in read-only `RunFinished`, after the edit transaction has been validated.

Both edit events follow these integrity and transaction rules:

- The loop reserves every `state.loop` field, including status, phase, counts, diagnostics, and pending projection results. A hook fails if it deletes or changes the progress record. Pruning does not decrease `completed_exchanges`, which counts model exchanges actually performed.
- History may be edited in `Ready`. If the original history was nonempty, at least one turn must remain. Message kinds must match their positions; tool calls and results must agree in count, order, ID, and name. Call identities must be nonempty and unique within one response. A hook may delete a complete step, or delete matching calls and results together, but not one side alone.
- In `Projection` or `Blocked`, history and recovery records must stay unchanged. Other fields may still be updated, but a hook cannot claim recovery by pruning unsettled calls, clearing pending results, or changing phase.
- `system_prompt` must remain renderable. Structural validation does not prove a summary is accurate, provider parameters are valid, or the tool catalog matches the registry; these are host responsibilities. The host also chooses how to archive significant tool output. Pruning never undoes external effects.
- Validation runs after all subscribers finish. If any subscriber throws or validation fails, all edits from that event are rolled back. Previously committed conversation and tool results remain. Callbacks should edit only `state`; external requests made by a callback cannot be rolled back with it.

Copy cost: with no subscribers, the loop only checks subscription count; it neither copies nor scans the whole state. With subscribers, each event takes one deep copy as a rollback backup shared by all its subscribers. On success, edits remain in the original object without another copy or move commit. On failure, a nonthrowing move assignment restores the backup. The `Ready` path validates history structure without serializing the whole state. Only in recovery phases does it construct JSON values for frozen history and nonempty pending results to check that recovery evidence was not changed. Subscribing to both edit events incurs one backup per event. Reliable rollback of arbitrary in-place edits cannot be achieved merely by moving the original object instead of backing it up.

For example, keep only the most recent step of the current turn after a tool batch settles:

```cpp
auto pruning = bus.subscribe<loop::EditOnStepFinished>(
    [](const loop::EditOnStepFinished& event) {
        auto& steps = event.state.turns.back().agent_loop_step;
        if (steps.size() > 1) {
            steps.erase(steps.begin(), steps.end() - 1);
        }
    });
```

This removes complete steps, including both tool calls and results. For business auditing or important output retention, define a summary and archival policy first. The same pruning can run in `EditOnRunFinished` when `event.state.loop->phase == model_io::LoopPhase::Ready`.

## Loop status and recoverability

`status` describes the lifetime and outcome of one invocation; `phase` describes a recovery boundary. They are not interchangeable: `Failed + Ready` permits continuation, while `Failed + Blocked` requires manual investigation. Admission changes status to `Running`. Terminal status is written before `EditOnRunFinished`. Recovery precedes admission; a new invocation starts only after old results have been projected successfully.

| Phase | Meaning and next `run()` behavior |
|---|---|
| `Ready` | No unsettled work. Validate history, then continue or accept new input. |
| `Model` | Model exchange in progress; response not committed. Validate history on recovery and do not treat a partial response as complete. |
| `Tools` | Tools may have effects, but complete returns were not saved. Next `run()` throws `RecoveryRequired(Tools)`; the host decides how to proceed. |
| `Projection` | Complete returns are in `pending_results`. Retry projection into candidate history, then clear the buffer. Do not rerun tools. |
| `Blocked` | Tool phase exited abnormally; effects are uncertain. Next `run()` throws `RecoveryRequired(Blocked)` for host investigation. |

Normal cleanup changes `Model` to `Ready`, unsettled `Tools` to `Blocked`, and leaves `Projection` unchanged. Full-state edits cannot alter these recovery judgments; a failed edit rolls back only the current event and changes the terminal outcome to `Failed`. Projection recovery commits candidate state atomically; failure retains old history and results. Successful recovery does not publish `EditOnStepFinished`, because it is not a newly executed batch. A host that needs to reorganize recovered history can do so at a later normal boundary.

Stop requests do not interrupt synchronous edits or validation: a valid edit finishes before stop is handled. A new stop request in `EditOnRunFinished` does not change an outcome already determined. Persist only after event transactions finish. There is no write-ahead log, so state markers cannot prove that all tool effects made before a process crash were recorded.

## Stop boundaries

Use an explicit `stop_token`: model waits are interruptible, but a tool batch that has started is not. A synchronous hook may call the host's `stop_source.request_stop()`, or another thread may request stop. Other threads must not read or write `state`.

- Stop before entry: leave `state` unchanged.
- Stop before a model request: do not start that request.
- Stop during a model request: send terminal cancellation through this `converse()` call's independent cancellation slot, interrupt the asynchronous wait, and wait for the model coroutine and its network tasks to exit. Return `Cancelled` without committing an incomplete response or executing its unfinished tool calls.
- Model response completes concurrently with stop: use the coroutine's actual outcome. If `converse()` returns a complete response, commit it; a final answer can still yield `Completed`, while a response with calls gets skipped results when stop is observed. An independent error completed before cancellation reaches transport yields `Failed`; a racing HTTP error can become `operation_aborted` and yield `Cancelled` after cancellation reaches transport.
- Stop before a batch starts: execute no tools and produce non-execution results marked `loop_skipped`.
- Stop during a batch: wait for the whole batch, including serial calls not yet started, and write back all results before returning `Cancelled`.
- During result commit: do not suspend, call external hooks, or react to cancellation.

The outer loop masks inherited Asio cancellation to protect the registry join; callers use the supplied `stop_token`. Model calls run on a private strand. A stop from another thread is posted to that strand before it fires this call's cancellation signal. The stop callback is registered after the child coroutine establishes its cancellation slot, preventing a lost request; late notifications cannot affect later calls.

The built-in Chat Completions and Responses streaming transports cancel and join both producer and consumer paths. They also stop residual reads after response consumption, preventing a server-held connection from leaving a background coroutine suspended. Cancellation does not trigger retries, and backoff waits can be interrupted.

Third-party models must honor `converse()`'s Asio terminal-cancellation contract and wait for their own spawned tasks before returning. The loop cannot safely kill synchronous blocking code or a provider that suppresses cancellation. A tool batch may still wait for external operations to finish. Do not simulate cancellation by stopping `io_context`, destroying dependencies, or unloading plugins.

Child processes that have returned session information may keep running; the loop does not terminate them automatically. The process toolset remains responsible for their subsequent state.

## Verification

`test_loop` uses an offline scripted model, the real `ToolRegistry`, and controllable tools to check the normal cycle, event order, writable-hook commit and rollback, preservation of tool-effect results, projection recovery after a JSON round trip, stop boundaries, exchange limits, finish-hook errors, and rejection of unanswered-call replay. It also covers interruption of a long-suspended model on a multithreaded executor, continuation of the same session after cancellation, and cancellation racing an independent model error. Exception cases include `operation_aborted` without a stop request, nonstandard exceptions, and recovery after repeated projection failures without changing old state or rerunning tools. `test_loop_model_cancellation` uses a local HTTP server to verify that both built-in adapters can cancel before a response and after streaming headers, and that the server observes connection closure. HTTP 503 cases verify that exhausted retries return `Failed` with HTTP diagnostics, do not commit partial responses, and publish exactly one finish event; request counts confirm the loop adds no retries. Full-state-hook tests cover event order, original-object identity, continuation after pruning, exception rollback, recovery-record protection, stop, and budget boundaries. The interactive experiment lives in `loop/example`; the older example remains.

### Host persistence checkpoints

ToolDispatchCheckpoint and ToolResultsCheckpoint are synchronous, read-only
boundaries. They expose the same borrowed-state restrictions as other observers.
The first runs even for a skipped batch and is conservative: a failed observer
leaves the run Blocked although dispatch has not begun. The second preserves a
complete pending-results buffer if saving fails before projection. A resumed
Projection state projects that buffer without redispatching tools.

StepFinished runs only after EditOnStepFinished returns and its final validation
succeeds. It does not run when the edit rolls back, during entry-time recovery,
or for a response with no calls. RunFinished remains the final read-only event;
because it logs observer errors, a host must separately latch required storage
failures. These notifications do not themselves perform IO or guarantee crash
durability. The core worker supplies that policy.
