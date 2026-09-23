# 交互式 loop 示例

`loop_deepseek_chat` 使用 `loop::run()`、DeepSeek 插件和完整 process 工具集。旧 `tools/example` 保留；新示例不复用它的循环体。工具目录和 skill 来自 ToolRegistry，确认仍使用工具框架的异步确认事件。

## 构建和启动

在仓库根目录构建镜像，进程实验只在容器内进行：

```sh
docker build -f docker/Dockerfile.test-context -t simplex-cpp-loop-test .
docker run --rm -it -e DEEPSEEK_API_KEY simplex-cpp-loop-test \
  /src/build/bin/loop_deepseek_chat
```

先在当前终端设置 `DEEPSEEK_API_KEY`；`-e DEEPSEEK_API_KEY` 传递变量，不把值写进命令。不要给实验容器挂载宿主工作区或 Docker socket。需要使用已安装版本时，改为 `/src/stage/bin/loop_deepseek_chat`。镜像默认启动说明页和 shell。

默认模型为 `deepseek-v4-flash`，可通过 `DEEPSEEK_MODEL` 覆盖；`DEEPSEEK_BASE_URL` 可指定兼容服务或本地协议夹具。不设置时使用 DeepSeek 插件默认地址。未提供环境变量时会询问 API key（终端输入会回显，推荐环境变量）。

选项：

- `--tools` / `--skill`：检查并显示工具目录或 skill，不需要密钥，不启动子进程。
- `--list-models`：显示在线模型目录及余额信息。
- `--yes`：自动批准工具调用；默认逐次确认，支持批准本工具、批准全部或拒绝后续请求。
- `--max-steps N`：每次 run 的模型交换预算，默认 12。
- `--effort high`：推理强度，默认 high；none/minimal 关闭 thinking。
- `--reasoning`：显示已完成响应中的完整推理区块，默认隐藏。
- `--log PATH`：框架错误日志，默认 `/tmp/loop-deepseek-chat.log`，追加写入。

REPL：`/tools`、`/skill`、`/sessions`、`/state`、`/continue`、`/help`、`/quit`。空行也退出。`/continue` 以 `has_message=false` 继续已有 turn，不伪造一条“继续”消息。默认不裁剪历史；`/state` 显示恢复阶段和待投影结果，可能较长。

## 输出与停止

所有交互内容按完整区块写到 stdout。模型回答、推理、工具调用、确认和工具结果各有标题；工具结果保留工具集原有的 stdout/stderr 标签，逐行缩进，绝不把子进程 stderr 当作示例自身的 stderr。不会解析工具的人类可读文本或声称两条流的跨流时间顺序。

不再把推理增量直接刷入终端：模型等待期间显示状态，完成后才显示可选推理和回答；取消／失败的部分响应不显示为完整回答。框架只记录 error 及以上到独立日志，避免调试信息穿插提示。控制字符包括 ESC、CR、NUL 显示为 `\xNN`，避免子进程进度条或 ANSI 转义覆盖标题；UTF-8 和换行保持可读。该转换只影响显示，模型历史保留原始内容。

输入和工具确认使用同一异步输入通道，等待用户时 io_context 仍可收集子进程输出。Ctrl-C 在 run 中请求 stop；模型等待可中断，已启动工具批次仍等待结算。确认提示处 Ctrl-C 拒绝当前及本轮剩余确认。在输入提示处 Ctrl-C 退出。SIGTERM 请求退出并停止当前 run。停止不会强杀已分发工具，长耗时工具可能延迟返回；再次按 Ctrl-C 不会绕过这个约束。

退出时调用 `terminate_all(false)`，保持执行器运行直到子进程工作完成。信号回调使用独立生命周期对象，取消后的延迟回调不引用已销毁的聊天协程。不要用停止执行器模拟取消。

## 建议交互实验

1. 请求“运行命令，stdout 输出 hello，stderr 输出 warning”：检查两种输出的标签、缩进和区块边界。
2. 请求“启动 cat，发送一行，再关闭 stdin”：检查跨轮会话和 `/sessions`。
3. 对工具确认回答 `n`：拒绝应成为工具结果，模型仍可继续回答。
4. 用 `--max-steps 1` 请求执行命令，再输入 `/continue`：工具不应重复执行。
5. 在模型等待期间 Ctrl-C，再 `/state` 和 `/continue`：应显示 cancelled/ready 并安全继续。
6. 请求输出带 ANSI 颜色和 CR 的内容：终端显示转义文本，不能覆盖提示。

镜像构建运行离线测试。`loop_example_container_smoke` 用本地 HTTP/SSE 服务和虚拟密钥验证真实插件、process 调用、预算后继续、拒绝及 Ctrl-C；不访问外部 provider。CTest 在运行时查找 Python 3；不在 Docker 内或没有 Python 3 时自动跳过，因此跨发行版运行已打包的测试树不会依赖构建镜像的解释器路径。真实 DeepSeek 交互由操作者在容器里完成。
