# loop

一个 harness 进程内只允许一个活跃 `loop::run()`，包括同步钩子执行和工具收尾时间。调用方保证此约束；本包没有 gate、队列、全局运行实例或独立 journal。registry 内部仍可按工具声明并行执行同一批次。

当前已实现正常循环、同步可写钩子、结果提交与恢复，以及可中断模型等待的 stop_token 停止机制。新增 [交互示例](example/README.md) 使用本包驱动 DeepSeek 和 process 工具集，旧示例保留。

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

`has_message = false` 时忽略 message（可以传 `{}`），继续最后一个 turn，不创建用户输入，不重跑已有工具调用。没有 turn 时返回 Failed。max_exchanges 必须大于零；每次模型响应计一次交换。最后一次响应带有工具调用时，仍完整处理该批次再报告 ExchangeLimit。

## 状态与恢复

`AgentInputState` 是唯一可持久化状态。新增可选 `loop` 字段记录当前状态、阶段、交换计数、错误，以及结果提交失败时的 `pending_results`。该字段是宿主元数据，不是 provider 请求参数。新增字段改变 C++ 布局，模型插件 ABI 已升至 5；旧插件需要重编译。旧 JSON 不含此字段时仍可读取。`RunResult` 只是此次调用摘要，无须另行持久化。

正常流程是：输入提交 → 模型请求 → 响应提交 → registry 批次执行 → 结果提交 → 下一次请求。没有工具调用的模型响应表示完成。

返回状态：Completed、Cancelled、ExchangeLimit、Failed。诊断使用 logging；工具输出及错误记录写入 dataclass，日志不作为恢复依据。常规结束会先保存 loop.status/error，运行可写的 EditOnRunFinished，再同步发布只读的 RunFinished。参数错误或前置取消发生在接纳运行前，不发布运行事件、不覆盖原有运行信息。

所有 `model.integrate()` 都在候选状态上执行，成功后以不抛异常的 move assignment 提交。工具返回的完整结果先移动到 `state.loop.pending_results`，再统一整合至候选对话；投影失败不会产生半份工具消息，下一次 run 先补写这些结果，再接收新输入或继续请求模型。此缓冲只存放尚未提交的结果，成功后清空，不保留第二份执行历史。

`LoopProgress::status` 使用 `model_io::LoopStatus` 枚举（Idle、Running、Completed、Cancelled、ExchangeLimit、Failed），`phase` 使用 `model_io::LoopPhase` 枚举（Ready、Model、Tools、Projection、Blocked）。JSON 保持原有小写名称（如 `exchange_limit`、`projection`），未知名称或错误类型在反序列化时抛出异常。恢复时，Tools/Blocked 表示工具结果不确定，`run()` 抛出 `loop::RecoveryRequired`，不自动重放或解决，由宿主决定如何核查和修复。Projection 必须带有待提交结果；验证所有工具调用与结果的数量、顺序、id/name 匹配后才能继续。外部导入的未回答调用同样拒绝自动重放。调用方不得篡改历史或恢复标记。

宿主可捕获 `RecoveryRequired` 并读取 `phase()` 区分 Tools 和 Blocked，再检查持久化状态与工具外部效果。不要直接把这一异常当成可自动重试的模型失败。当前 `LoopProgress` 与对话一同保存在唯一的 `AgentInputState` 中；后续可评估把宿主恢复元数据移到独立的可持久化 dataclass，并让模型边界只接收对话数据，以免每次调整恢复协议都影响模型插件 ABI。本次保持现有状态协议。

这是正常进程存续期间的恢复协议，不是写前日志：宿主负责何时保存 dataclass，不能承诺任意崩溃、断电或内存耗尽后都能恢复已发生的副作用。工具未报告的外部细节也无法由 loop 推断。

## 异常处理

`run()` 把普通运行异常转换为 `RunResult`，调用方通常检查 `result.status` 和 `result.error`。前一轮的 `Tools`／`Blocked` 是例外：接纳被拒绝，抛出携带原始 phase 的 `RecoveryRequired`，状态不变，交给宿主处理。异常处理分为三层：

1. `converse_interruptibly()` 等待模型协程退出，异常时关闭本次取消桥接并原样重抛，不在这里判断失败原因。
2. 模型等待位置只识别取消：必须同时满足 `stop.stop_requested()` 和 `boost::system::system_error` 的错误码为 `operation_aborted`，才返回 `Cancelled`。单独出现任一条件都不足以认定取消。
3. 运行主体先单独重抛 `RecoveryRequired`，再以 `catch (...)` 接收其余异常，包括参数校验、投影恢复、模型调用、历史整合、registry 分发以及同步钩子的失败，将结果设为 `Failed` 并记录日志。`std::exception` 使用 `what()` 作为诊断，其他异常使用 `unknown exception`。

内置 Chat Completions 和 Responses 适配器调用 `endpoint::complete`。取消一旦传入传输层，遇到的 HTTP 错误也可能转为 `operation_aborted`；因此 HTTP 失败与停止竞争时，实际完成的异常决定最终结果。若独立错误先于取消生效而完成，则报告 `Failed`；若终止取消已传播到传输层，完成为 `operation_aborted`，则报告 `Cancelled`。不可重试的 HTTP 错误或重试耗尽若原样抛出 `HttpRequestException`，由第三层处理。loop 不自行重试模型请求。工具通过返回值报告的错误仍是工具结果，不自动导致整个 loop 失败；registry 向外抛出的异常才进入 loop 的失败路径。

失败后的状态取决于发生位置：

| 位置 | 状态与后续行为 |
|---|---|
| 接纳运行前的普通校验或投影恢复失败 | 返回 `Failed`，不创建新的进度记录，不发布 `RunFinished`；此前成功恢复的旧结果可能已经提交 |
| 前一轮为 Tools／Blocked | 抛出 `RecoveryRequired`，提供原始 phase；不自动重放工具，不接纳本轮 |
| 模型请求 | 不提交失败请求的部分响应，`phase` 从 `Model` 恢复为 `Ready` |
| 工具阶段未能取得并保存完整结果 | `phase` 从 `Tools` 改为 `Blocked`，拒绝自动重放，已发生的外部副作用不回滚 |
| 工具结果投影 | 保留 `Projection` 和完整 `pending_results`，后续只重试投影 |
| 同步钩子 | 按下节所述保留已提交数据；需要时先补齐未执行工具结果，再结束运行 |
| `EditOnRunFinished` | 撤销此次状态编辑，更新为 `Failed` 并追加诊断，继续发布 `RunFinished` |
| `RunFinished` | 已完成终态记录；订阅者异常仅记录日志，不改变返回值或持久化状态，也不再次发布结束事件 |

运行已接纳时，loop 在发布 `RunFinished` 前保存终态和错误。`Failed` 表示本次运行失败，不表示整个输入、对话历史或外部工具副作用已回滚。

`run()` **不是 `noexcept` 边界**：参数或协程帧构造、协程初始化，以及生成诊断、写入终态或记录日志时的二次异常仍可能向调用方传播。宿主在等待 `run()` 时仍需保留最外层异常处理；不能把“未正常返回”当作可以自动重跑工具的依据。

## 同步钩子

宿主如需把一组事件处理函数组织为有状态插件，可实现顶层公共接口
[`LoopHookInterface`](include/loop/hook_interface.hpp)，并将实例交给会话级
[`LoopHookRegistry`](include/loop/hook_registry.hpp)。registry 接收与 `run()`
相同的同步 bus，以插件名称管理实例与订阅周期：`add()` 注册、`set()`
新增或替换、`get()` 查询、`remove()` 和 `clear()` 解除订阅。插件的
`subscribe()` 应返回全部订阅句柄。底层 `LoopHookBinding` 同时持有插件
实例和订阅句柄，销毁时先断开监听。绑定、解绑需与 `run()` 串行化，
bus 必须比 registry 长寿。

```cpp
eventbus::EventBus bus;
loop::LoopHookRegistry hooks(bus);
hooks.add(std::make_shared<MyLoopHook>());
// 让 bus 和 hooks 在所有 loop::run() 调用期间保持存活。
```

插件实例可保存进程内状态；需要跨进程恢复的数据仍应写入
`AgentInputState`。内建插件目录位于 [`intrinsic/`](intrinsic/)，
不通过 `extensions` 声明。内建插件的 YAML 配置在构造实例时读取，
并随 `cmake --install` 安装到 `bin/schemas/loop/<插件名>/config.yaml`；
配置格式、路径覆盖和错误处理见该目录的 README。

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
| EditOnStepFinished | 原地编辑完整 state；前一事件成功返回后，下一次模型请求前 |
| EditOnRunFinished | 原地编辑完整 state；终态已保存，RunFinished 之前 |
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

事件本身为 const，其中声明为可写的对象引用允许同步修改。要更新插件状态，可在候选 extras 的既有 external_status/events 区域按 dataclass 协议处理；不得在回调中异步修改候选对象。

钩子异常会结束本次运行。模型响应提交后、工具执行前的钩子失败，会为已声明的调用补上“未执行”结果，闭合调用关系。已执行工具的结果不会因后续钩子失败而回滚。

EventBus 的一个订阅者抛异常会阻止本次事件的后续订阅者执行。`EditOnRunFinished` 可改变终态并在失败时回滚其状态编辑；随后 `RunFinished` 只通知最终结果。它的订阅者若抛异常，后续订阅者不再执行，异常被记录，先前观察者和 `run()` 调用方看到相同的终态。

## 完整状态编辑与裁剪

`EditOnStepFinished` 和 `EditOnRunFinished` 的 `state` 都是调用方传入的同一个 `AgentInputState&`。多个订阅者按顺序直接编辑它，后面的订阅者能看到前面的修改。引用只在回调期间有效，禁止保存引用、异步访问、从其他线程观察中间态或重入 `run()`。订阅变更应与 loop 执行串行化；在无订阅检查之后才添加的订阅者不保证参与当前边界。

`EditOnStepFinished` 在 `ToolResultsCommitted` 的全部订阅者成功返回后运行，每个已结算工具批次一次，包括因停止而生成未执行结果的批次、耗尽预算的最后一批。无工具调用的响应、入口处恢复旧结果，以及前面的观察钩子抛异常时，不发布它。适合工具历史裁剪、旧 turn 摘要和下一轮模型上下文整理。修改后的状态供下一次模型请求使用。

`EditOnRunFinished` 在终态保存后运行，每次已接纳的 invocation 一次，覆盖 Completed、Cancelled、ExchangeLimit 和 Failed。它可整理最终历史、更新宿主摘要或持久化元数据；只读 `result` 表示进入该钩子时的结果。未接纳的参数错误、前置取消或入口恢复失败不发布它。若该钩子失败，loop 撤销本次编辑，追加带 `EditOnRunFinished:` 的诊断并更新为 Failed，仍发布一次 `RunFinished`。因此，最终保存状态通常放在只读 `RunFinished` 中，而不是在编辑事务尚未验证时保存。

两个事件共用以下完整性与事务规则：

- `state.loop` 全部字段由 loop 保留，包括状态、阶段、计数、诊断及待投影结果；钩子删除或改动它会失败。裁剪不会减少 `completed_exchanges`，这个计数记录实际执行的模型交换数。
- 在 `Ready` 阶段可以编辑历史。原历史非空时至少保留一个 turn；消息种类必须符合所在位置，工具调用和结果必须数量、顺序、id/name 对应，调用身份非空且同一响应内不重复。可以删除完整 step，也可以同时删除对应调用和结果，不能只删除一侧。
- 在 `Projection` 或 `Blocked` 阶段，历史和恢复记录必须保持原值。此时仍可更新其他字段，但不能通过裁剪未结算调用、清空待投影结果或改阶段来宣称恢复成功。
- system_prompt 必须可渲染。结构校验不证明摘要真实、provider 参数有效或工具目录与 registry 一致；这些由宿主负责。保留重要工具输出的归档策略也由宿主决定，裁剪不会撤销外部副作用。
- 全部订阅者执行完后统一校验；任一订阅者抛异常或校验失败，撤销该事件所有订阅者的编辑。之前已提交的对话和工具结果保留。回调应只修改 state；自身发出的外部请求等副作用无法随状态一起回滚。

复制开销：无订阅者时只查询订阅数，不复制或扫描整个状态。有订阅者时，每个事件只深拷贝一次用于回滚，多个订阅者共用该备份；成功时修改留在原对象中，不再复制或 move 提交；失败时通过不抛异常的 move assignment 恢复。Ready 路径校验历史结构，不序列化整个状态；仅在异常恢复阶段对冻结历史和非空待投影结果构造 JSON 值比较，确保恢复证据没有被改写。两个编辑事件都订阅时，各自承担一次备份成本。任意原地修改需要可靠回滚，就不能只移动原对象来替代备份。

例如，在工具批次结算后仅保留当前 turn 最近的一步：

```cpp
auto pruning = bus.subscribe<loop::EditOnStepFinished>(
    [](const loop::EditOnStepFinished& event) {
        auto& steps = event.state.turns.back().agent_loop_step;
        if (steps.size() > 1) {
            steps.erase(steps.begin(), steps.end() - 1);
        }
    });
```

这里按完整 step 裁剪，工具调用和结果一起删除。涉及业务审计或需要保留输出的场景，应先制定摘要和归档策略。也可以订阅 `EditOnRunFinished`，只在 `event.state.loop->phase == model_io::LoopPhase::Ready` 时执行相同裁剪。

## 循环状态与可恢复性

`status` 表示一次 invocation 的生命周期和结果，`phase` 表示恢复边界，两者不能相互替代。例如 `Failed + Ready` 允许继续，而 `Failed + Blocked` 需要人工核查。接纳运行时状态变为 Running；终态写入发生在 EditOnRunFinished 之前。恢复先于接纳，成功投影旧结果之后才开始新的 invocation。

| phase | 含义及下次 run 的行为 |
|---|---|
| Ready | 没有未结算工作；验证历史后可以继续或接纳新输入 |
| Model | 模型交换中，响应尚未提交；恢复时验证历史，不把部分响应视作已完成 |
| Tools | 工具可能已有副作用但未保存完整返回；下次 `run()` 抛出 `RecoveryRequired(Tools)`，宿主决定处理方式 |
| Projection | 完整返回已保存在 pending_results；先在候选历史上重试投影，成功后清空缓冲，不能重跑工具 |
| Blocked | 工具阶段异常退出，副作用不确定；下次 `run()` 抛出 `RecoveryRequired(Blocked)`，需宿主核查 |

运行正常收尾时 Model 转回 Ready，未结算的 Tools 转为 Blocked，Projection 保持原样。完整状态编辑不能改变这些恢复判断；编辑失败只撤销当前事件，终态变为 Failed。恢复投影在候选状态上整体提交，失败保留旧历史及结果；恢复成功也不发布 EditOnStepFinished，避免把它当成新执行的批次。宿主如需对恢复后的历史做整理，可在后续正常边界处理。

停止请求不会打断同步编辑和校验：合法编辑完成后才处理停止。EditOnRunFinished 中新发出的停止请求不改写已确定的结果。持久化只应在事件事务完成后进行；这里没有写前日志，不能由状态标记推断进程崩溃前所有工具副作用都已被记录。

## 停止边界

使用显式 stop_token 请求停止：模型等待可中断，已经启动的工具批次不可中断。可以在同步钩子内调用宿主 stop_source.request_stop()，或从其他线程请求停止；其他线程不能读写 state。

- 开始前已停止：不修改 state。
- 模型请求前停止：不发起请求。
- 模型请求中停止：向当前 converse 的独立取消槽发送 terminal cancellation，中断异步等待，并等待模型协程及其网络任务完成退出。返回 Cancelled，不提交不完整响应，不执行其中尚未完成的工具调用。
- 模型响应与取消同时完成：以协程实际完成结果为准。若 converse 成功返回完整响应，仍提交它；最终回答可返回 Completed，带调用的响应则在观察到停止后补齐未执行结果。独立错误在取消传入传输层之前完成时返回 Failed；取消已传入传输层后，竞争中的 HTTP 错误可能转为 `operation_aborted` 并返回 Cancelled。
- 批次开始前停止：不执行任何工具，生成带 loop_skipped 标记的非执行结果。
- 批次执行中停止：整个批次完成并写回结果后，返回 Cancelled，包括尚未启动的串行调用。
- 结果提交期间不挂起、不调用外部钩子、不响应取消。

外层循环屏蔽继承的 Asio 取消，以保护 registry 的 join；调用方使用传入的 stop_token。模型调用运行在私有 strand 上，跨线程停止会投递到该 strand，再触发本次调用的取消信号。停止回调在子协程建立取消槽之后注册，避免请求丢失；迟到通知不影响后续调用。

内置 Chat Completions 和 Responses 的流式传输同时取消并等待生产、消费两条路径退出。完成响应消费后也会结束残留读取，避免服务端保持连接导致后台协程悬挂；取消不会触发重试，退避等待也可以中断。

第三方模型必须遵守 converse 的 Asio terminal cancellation 契约，并在返回前等待自身派生任务退出。同步阻塞代码或主动屏蔽取消的 provider 无法由 loop 安全强杀。工具批次仍可能等待外部操作完成。不得用停止 io_context、销毁依赖或卸载插件替代取消。

已返回 session 信息的子进程可以继续运行，loop 不自动终止它。执行进程的后续状态仍由 process 工具集管理。

## 验证

`test_loop` 使用离线脚本模型、实际 ToolRegistry 和可控工具执行正常循环，验证事件顺序、可写钩子提交/回滚、工具副作用结果保留、JSON 往返后的投影恢复、停止边界、预算耗尽、结束钩子错误和禁止重放未回答调用。另外覆盖多线程 executor 上中断长期挂起的模型、取消后继续同一会话，以及取消与独立模型错误同时发生的情况。异常路径另覆盖未请求停止时的 `operation_aborted`、非标准异常，以及连续投影失败后保留原状态并恢复，确保不重复执行工具。`test_loop_model_cancellation` 使用本地 HTTP 服务验证两种内置适配器在请求无响应和已发出流式响应头时可被取消，且服务端观察到连接关闭。另外用 HTTP 503 验证两种适配器各自重试耗尽后返回 `Failed`，保留 HTTP 诊断、不提交部分响应，并只发布一次结束事件；请求计数验证 loop 没有增加额外重试。完整状态钩子的测试覆盖事件顺序、原对象身份、裁剪后继续、异常回滚、恢复记录保护以及停止和预算边界。交互实验使用独立的 `loop/example`，旧示例保留。
