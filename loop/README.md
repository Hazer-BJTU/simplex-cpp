# loop

一个 harness 进程内只允许一个活跃 `loop::run()`，包括同步钩子执行和工具收尾时间。调用方保证此约束；本包没有 gate、队列、全局运行实例或独立 journal。registry 内部仍可按工具声明并行执行同一批次。

当前已实现正常循环、同步可写钩子、结果提交与恢复，以及可中断模型等待的 stop_token 停止机制。现有示例没有迁移。

## 使用

链接 `loop_lib`，包含 `loop/loop.hpp` 和需要订阅的 `loop/events.hpp`：

```cpp
model_io::AgentInputState state;
std::stop_source stop;
model_io::MessageItem input;
input.type = model_io::MessageItemType::UserInput;
input.role = "user";
// 填入 input.content；按宿主策略设置 state.system_prompt 和 state.tools。
auto result = co_await loop::run(
    model, registry, bus, executor, state,
    true, std::move(input), {.max_exchanges = 32}, stop.get_token());
```

所有依赖显式注入。loop 不自行加载模型、注册工具、注入 skill 或读取全局总线。依赖中的引用和 state 必须存活到返回；输入和选项按值进入惰性协程帧。模型、registry 和 bus 的生命周期由宿主负责。

`has_message = false` 时忽略 message（可以传 `{}`），继续最后一个 turn，不创建用户输入，不重跑已有工具调用。没有 turn 时返回 Failed。max_exchanges 必须大于零；每次模型响应计一次交换。最后一次响应带有工具调用时，仍完整处理该批次再报告 StepLimit。

## 状态与恢复

`AgentInputState` 是唯一可持久化状态。新增可选 `loop` 字段记录当前状态、阶段、交换计数、错误，以及结果提交失败时的 `pending_results`。该字段是宿主元数据，不是 provider 请求参数。新增字段改变 C++ 布局，模型插件 ABI 已升至 5；旧插件需要重编译。旧 JSON 不含此字段时仍可读取。`RunResult` 只是此次调用摘要，无须另行持久化。

正常流程是：输入提交 → 模型请求 → 响应提交 → registry 批次执行 → 结果提交 → 下一次请求。没有工具调用的模型响应表示完成。

返回状态：Completed、Cancelled、StepLimit、Failed。诊断使用 logging；工具输出及错误记录写入 dataclass，日志不作为恢复依据。常规结束会先保存 loop.status/error，再同步发布 RunFinished。参数错误或前置取消发生在接纳运行前，不发布运行事件、不覆盖原有运行信息。

所有 `model.integrate()` 都在候选状态上执行，成功后以不抛异常的 move assignment 提交。工具返回的完整结果先移动到 `state.loop.pending_results`，再统一整合至候选对话；投影失败不会产生半份工具消息，下一次 run 先补写这些结果，再接收新输入或继续请求模型。此缓冲只存放尚未提交的结果，成功后清空，不保留第二份执行历史。

`LoopProgress::status` 使用 `model_io::LoopStatus` 枚举（Idle、Running、Completed、Cancelled、StepLimit、Failed），`phase` 使用 `model_io::LoopPhase` 枚举（Ready、Model、Tools、Projection、Blocked）。JSON 保持原有小写名称（如 `step_limit`、`projection`），未知名称或错误类型在反序列化时抛出异常。恢复时，tools/blocked 表示工具结果不确定，拒绝自动执行。projection 必须带有待提交结果；验证所有工具调用与结果的数量、顺序、id/name 匹配后才能继续。外部导入的未回答调用同样拒绝自动重放。调用方不得篡改历史或恢复标记。

这是正常进程存续期间的恢复协议，不是写前日志：宿主负责何时保存 dataclass，不能承诺任意崩溃、断电或内存耗尽后都能恢复已发生的副作用。工具未报告的外部细节也无法由 loop 推断。

## 异常处理

`run()` 把运行主体中的异常转换为 `RunResult`，调用方通常检查 `result.status` 和 `result.error`。异常处理分为三层：

1. `converse_interruptibly()` 等待模型协程退出，异常时关闭本次取消桥接并原样重抛，不在这里判断失败原因。
2. 模型等待位置只识别取消：必须同时满足 `stop.stop_requested()` 和 `boost::system::system_error` 的错误码为 `operation_aborted`，才返回 `Cancelled`。单独出现任一条件都不足以认定取消。
3. 运行主体的外层 `catch (...)` 接收其余异常，包括参数校验、恢复、模型调用、历史整合、registry 分发以及同步钩子的失败，将结果设为 `Failed` 并记录日志。`std::exception` 使用 `what()` 作为诊断，其他异常使用 `unknown exception`。

内置 Chat Completions 和 Responses 适配器调用 `endpoint::complete`。传输取消会转换为 `operation_aborted`；不可重试的 HTTP 错误或重试耗尽则原样抛出 `HttpRequestException`，由第三层处理。协议层 API 异常也走第三层。loop 不自行重试模型请求，也不会仅因为同时收到停止请求就把独立错误改报为取消。工具通过返回值报告的错误仍是工具结果，不自动导致整个 loop 失败；registry 向外抛出的异常才进入 loop 的失败路径。

失败后的状态取决于发生位置：

| 位置 | 状态与后续行为 |
|---|---|
| 接纳运行前的校验或恢复 | 返回 `Failed`，不创建新的进度记录，不发布 `RunFinished`；此前成功恢复的旧结果可能已经提交 |
| 模型请求 | 不提交失败请求的部分响应，`phase` 从 `Model` 恢复为 `Ready` |
| 工具阶段未能取得并保存完整结果 | `phase` 从 `Tools` 改为 `Blocked`，拒绝自动重放，已发生的外部副作用不回滚 |
| 工具结果投影 | 保留 `Projection` 和完整 `pending_results`，后续只重试投影 |
| 同步钩子 | 按下节所述保留已提交数据；需要时先补齐未执行工具结果，再结束运行 |
| `RunFinished` | 更新返回值及持久化状态为 `Failed`，追加诊断，不再次发布结束事件 |

运行已接纳时，loop 在发布 `RunFinished` 前保存终态和错误。`Failed` 表示本次运行失败，不表示整个输入、对话历史或外部工具副作用已回滚。

`run()` **不是 `noexcept` 边界**：参数或协程帧构造、协程初始化，以及生成诊断、写入终态或记录日志时的二次异常仍可能向调用方传播。宿主在等待 `run()` 时仍需保留最外层异常处理；不能把“未正常返回”当作可以自动重跑工具的依据。

## 同步钩子

bus 使用显式注入的同步 EventBus。回调按订阅顺序执行，回调返回后才继续循环。事件引用仅在回调期间有效，不得保存、从外部别名修改当前 state 或重入 run。

| 事件 | 权限与时机 |
|---|---|
| RunStarted | 只读；运行接纳后 |
| BeforeInput | 可写待提交用户消息；仍须保持 UserInput 且不含工具调用/返回 |
| InputCommitted | 只读；用户消息已提交 |
| BeforeModel | 可写候选 system_prompt、tools、extras；历史与恢复信息只读 |
| ModelCommitted | 只读；模型响应已提交 |
| BeforeToolBatch | 只读调用列表；工具尚未启动，可通过宿主 stop_source 请求停止 |
| ToolResultsCommitted | 只读；结果已全部写回对话，包括跳过的调用 |
| RunFinished | 只读；终态已保存 |

BeforeInput 和 BeforeModel 的多个订阅者依次修改同一候选数据；任何订阅者抛异常，该阶段修改不提交。模型上下文提交前会验证 prompt 可渲染。目录内容、provider 参数语义及与 registry 的一致性由宿主策略负责，loop 不擅自改写。

示例：

```cpp
auto subscription = bus.subscribe<loop::BeforeModel>(
    [](const loop::BeforeModel& event) {
        event.context.extras = nlohmann::json{{"custom_parameter", 42}};
    });
```

此同步约束针对 loop 钩子；既有 registry 的工具授权仍使用其原有异步机制，loop 会等待整个工具执行路径。

事件本身为 const，受控的候选对象引用可写。要更新插件状态，可在候选 extras 的既有 external_status/events 区域按 dataclass 协议处理；不得在回调中异步修改候选对象。

钩子异常会结束本次运行。模型响应提交后、工具执行前的钩子失败，会为已声明的调用补上“未执行”结果，闭合调用关系。已执行工具的结果不会因后续钩子失败而回滚。

EventBus 的一个订阅者抛异常会阻止本次事件的后续订阅者执行。RunFinished 失败时更新 state 和返回值为 Failed，记录日志，不再次广播结束事件；先前已通知的订阅者应以最终 state/返回值为准。

## 停止边界

使用显式 stop_token 请求停止：模型等待可中断，已经启动的工具批次不可中断。可以在同步钩子内调用宿主 stop_source.request_stop()，或从其他线程请求停止；其他线程不能读写 state。

- 开始前已停止：不修改 state。
- 模型请求前停止：不发起请求。
- 模型请求中停止：向当前 converse 的独立取消槽发送 terminal cancellation，中断异步等待，并等待模型协程及其网络任务完成退出。返回 Cancelled，不提交不完整响应，不执行其中尚未完成的工具调用。
- 模型响应与取消同时完成：以协程实际完成结果为准。若 converse 成功返回完整响应，仍提交它；最终回答可返回 Completed，带调用的响应则在观察到停止后补齐未执行结果。与停止同时发生的独立模型错误仍返回 Failed。
- 批次开始前停止：不执行任何工具，生成带 loop_skipped 标记的非执行结果。
- 批次执行中停止：整个批次完成并写回结果后，返回 Cancelled，包括尚未启动的串行调用。
- 结果提交期间不挂起、不调用外部钩子、不响应取消。

外层循环屏蔽继承的 Asio 取消，以保护 registry 的 join；调用方使用传入的 stop_token。模型调用运行在私有 strand 上，跨线程停止会投递到该 strand，再触发本次调用的取消信号。停止回调在子协程建立取消槽之后注册，避免请求丢失；迟到通知不影响后续调用。

内置 Chat Completions 和 Responses 的流式传输同时取消并等待生产、消费两条路径退出。完成响应消费后也会结束残留读取，避免服务端保持连接导致后台协程悬挂；取消不会触发重试，退避等待也可以中断。

第三方模型必须遵守 converse 的 Asio terminal cancellation 契约，并在返回前等待自身派生任务退出。同步阻塞代码或主动屏蔽取消的 provider 无法由 loop 安全强杀。工具批次仍可能等待外部操作完成。不得用停止 io_context、销毁依赖或卸载插件替代取消。

已返回 session 信息的子进程可以继续运行，loop 不自动终止它。执行进程的后续状态仍由 process 工具集管理。

## 验证

`test_loop` 使用离线脚本模型、实际 ToolRegistry 和可控工具执行正常循环，验证事件顺序、可写钩子提交/回滚、工具副作用结果保留、JSON 往返后的投影恢复、停止边界、预算耗尽、结束钩子错误和禁止重放未回答调用。另外覆盖多线程 executor 上中断长期挂起的模型、取消后继续同一会话，以及取消与独立模型错误同时发生的情况。`test_loop_model_cancellation` 使用本地 HTTP 服务验证两种内置适配器在请求无响应和已发出流式响应头时可被取消，且服务端观察到连接关闭。现有示例不迁移。
