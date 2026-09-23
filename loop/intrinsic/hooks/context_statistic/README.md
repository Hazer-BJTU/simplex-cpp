# ContextStatisticHook

`ContextStatisticHook` records provider-reported token usage in
`AgentInputState::extras.external_status.context_statistic`. The hook holds
only its configured context window; all counters live in the session state
and survive JSON round-trips and later history pruning. It subscribes to
`BeforeModel`, `EditOnStepFinished`, and `EditOnRunFinished`. Tool-bearing
responses are accounted after their results are committed; a final response
without tool calls is accounted at run finish. The within-run marker prevents
the finish hook from counting an already recorded step twice.

Construct it from `schemas/config.yaml` using
`ContextStatisticHook::from_config()` and add the shared instance to the
session's `LoopHookRegistry`:

```cpp
hooks.add(loop::intrinsic::ContextStatisticHook::from_config());
```

The YAML's only option is a positive integer
`context_window_tokens`. Change it and construct a new hook to use another
window without recompiling. The file installs to
`<prefix>/bin/schemas/loop/context_statistic/config.yaml`; the common
`SIMPLEX_LOOP_HOOK_SCHEMA_DIR` override also applies.

The status slot is flat. All token counts are nonnegative integers. `cache_hit`
is a subset of `prompt`, so `total` is `prompt + generated`, not their sum with
`cache_hit`.

| Field | Meaning |
|---|---|
| `context_window_tokens` | Configured window length. |
| `max_exchange_{prompt,generated,cache_hit,total}_tokens` | Maximum of each field over exchanges with provider usage; the maxima may come from different exchanges. |
| `last_exchange_{prompt,generated,cache_hit,total}_tokens` | Most recent exchange's provider usage, or `null` if it reported none. |
| `cumulative_exchange_{prompt,generated,cache_hit,total}_tokens` | Sum over exchanges with provider usage, including exchanges later pruned from history. |
| `last_window_remaining_ratio` | `(context_window_tokens - last_exchange_total_tokens) / context_window_tokens`; positive means room remains, negative means the last exchange exceeded the configured window, `null` if usage is absent. This uses actual output, not a reserved maximum-output budget. |
| `average_cache_hit_probability` | Cumulative cache-hit tokens divided by cumulative prompt tokens, weighted by prompt size; `null` if no prompt tokens were reported. Providers that report usage without cache details contribute a zero `cache_hit`. |
| `estimated_zero_request_tokens` | `ceil((rendered system-prompt UTF-8 bytes + serialized name/description/parameters bytes of tool definitions) / 4)`. An estimate of fixed cost before any user message, excluding provider wrappers and tokenizer-specific overhead. Recomputed from the context used for the latest exchange. |
| `sampled_exchange_count` | Number of exchanges with reported usage. |
| `accounted_exchanges_in_run` | Internal run-local marker that prevents duplicate accumulation. |

The hook writes only its own `external_status` slot through
`model_io::sync_external_status`; other extras fields and writers' slots are
preserved. It validates `cache_hit <= prompt` and refuses counter overflow.
Register it before a hook that prunes the current step in either edit event:
once a response has been removed, its usage cannot be reconstructed. No
tokenizer or provider request is performed by this hook.
