# Interactive loop example

`loop_deepseek_chat` uses `loop::run()`, the DeepSeek plugin, and the full process toolset. The older `tools/example` remains available; this example does not reuse its loop body. Tool catalogs and skills come from `ToolRegistry`. Tool confirmation still uses the tool framework's asynchronous confirmation event.

## Build and launch

Build the image from the repository root and run process experiments only inside the container:

```sh
docker build -f docker/Dockerfile.test-context -t simplex-cpp-loop-test .
docker run --rm -it -e DEEPSEEK_API_KEY simplex-cpp-loop-test \
  /src/build/bin/loop_deepseek_chat
```

Set `DEEPSEEK_API_KEY` in the current terminal first. `-e DEEPSEEK_API_KEY` passes the variable without putting its value in the command. Do not mount the host workspace or Docker socket into the experiment container. To use the installed version, run `/src/stage/bin/loop_deepseek_chat` instead. By default, the image opens an instructions page and a shell.

The default model is `deepseek-v4-flash`; override it with `DEEPSEEK_MODEL`. `DEEPSEEK_BASE_URL` selects a compatible service or local protocol fixture; if unset, the DeepSeek plugin uses its default URL. If the API key is absent, the program prompts for it. Terminal input is echoed, so the environment variable is preferred.

Options:

- `--tools` / `--skill`: inspect the tool catalog or skill without a key or child process.
- `--list-models`: show the online model catalog and balance information.
- `--yes`: approve tool calls automatically. By default, each call needs confirmation; you can approve one tool, approve all, or reject subsequent requests.
- `--max-exchanges N`: model-exchange budget per run; default 12.
- `--effort high`: reasoning effort; default `high`. `none` and `minimal` disable thinking.
- `--reasoning`: display complete reasoning blocks from finished responses; hidden by default.
- `--log PATH`: append framework error logs to this file; default `/tmp/loop-deepseek-chat.log`.

REPL commands are `/tools`, `/skill`, `/sessions`, `/state`, `/continue`, `/help`, and `/quit`. An empty line also exits. `/continue` uses `has_message=false` to resume an existing turn without fabricating a "continue" message. History is not pruned by default. `/state` shows the recovery phase and pending projection results, which may be lengthy.

## Output and stopping

All interactive content is written to stdout in complete blocks. Model answers, reasoning, tool calls, confirmations, and tool results have separate headings. Tool results preserve the toolset's stdout/stderr labels and indent each line; child-process stderr is never treated as the example program's own stderr. The example neither parses human-readable tool output nor claims a cross-stream time order between stdout and stderr.

Reasoning increments are not printed directly while streaming. A status is shown during the model wait, followed by optional reasoning and the answer only after completion. Partial responses from cancellation or failure are not presented as complete answers. The framework writes only error-level and higher messages to a separate log, keeping diagnostics out of prompts. Control characters, including ESC, CR, and NUL, are displayed as `\xNN` so progress bars or ANSI escapes from child processes cannot overwrite headings. UTF-8 and newlines remain readable. This conversion affects display only; model history keeps the original content.

Input and tool confirmation share one asynchronous input channel, so `io_context` can still collect child-process output while waiting for the user. Ctrl-C during a run requests stop: model waits can be interrupted, while a started tool batch must settle. At a confirmation prompt, Ctrl-C rejects the current and remaining confirmations for that round. At an input prompt, Ctrl-C exits. SIGTERM requests exit and stops the current run. Stop does not forcibly kill dispatched tools, so a long-running tool may delay return; another Ctrl-C does not bypass that rule.

On exit, the example calls `terminate_all(false)` and keeps the executor running until child-process work finishes. Signal callbacks use a separately owned lifetime object; late callbacks after cancellation do not refer to a destroyed chat coroutine. Do not stop the executor to imitate cancellation.

## Suggested interactive experiments

1. Ask it to run a command that writes `hello` to stdout and `warning` to stderr. Inspect the labels, indentation, and block boundaries.
2. Ask it to start `cat`, send a line, then close stdin. Check the cross-turn session and `/sessions`.
3. Reply `n` to a tool confirmation. The rejection should become a tool result, and the model should still be able to answer.
4. Request a command with `--max-exchanges 1`, then enter `/continue`. The tool must not run twice.
5. Press Ctrl-C during a model wait, then inspect `/state` and enter `/continue`. The state should show cancelled/ready and permit safe continuation.
6. Ask for output containing ANSI colors and CR. The terminal should show escaped text without overwriting the prompt.

The image build runs offline tests. `loop_example_container_smoke` uses a local HTTP/SSE service and a dummy key to exercise the real plugin, process calls, continuation after budget exhaustion, rejection, and Ctrl-C; it never contacts an external provider. CTest locates Python 3 at runtime and skips this test outside Docker or without Python 3, so a packaged test tree does not depend on the build image's interpreter path on other distributions. An operator performs real DeepSeek interaction inside the container.
