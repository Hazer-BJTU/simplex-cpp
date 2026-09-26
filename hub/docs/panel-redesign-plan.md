# Hub 面板重构方案：框架与技术选型

> 状态：**提案，等待确认**。本文只做选型与路线决策，不含代码改动。
> 约束：C++ 侧（`core/`、`simplex_worker`、worker 协议）**保持原样**，但 hub↔面板协议要预留扩展空间。

---

## 0. 结论摘要

| 决策项 | 选型 | 一句话理由 |
| --- | --- | --- |
| 前端框架 | **React 19 + TypeScript + Vite** | 生态最全：markdown、高亮、无头可访问组件、长列表全部有成熟方案 |
| 前端样式 | **Tailwind CSS v4 + Radix Primitives** | 设计 token 体系化；弹窗/菜单的键盘与焦点行为交给 Radix，不再手写 |
| 前端状态 | **Zustand** | 1KB，selector 订阅避免全量重渲染，可在 WebSocket handler 里直接读写 |
| Markdown | **react-markdown + remark-gfm + rehype-highlight** | 生成 React 元素树而**不是** HTML 字符串，天然无 XSS |
| 图标 | **lucide-react** | 按需引入的 SVG 图标，替换当前纯文字按钮 |
| 后端 | **TypeScript**，由 Node 直接运行 | `tsc` 只做类型检查、从不产出；`engines` 提到 ≥22.18 换取零构建 |
| 协议契约 | **`hub/shared/` 共享类型 + 运行时 guard + 能力协商** | hub 与面板同一份定义，扩展时两端不会漂移 |
| 后端测试 | **Vitest**（断言继续用 `node:assert`） | 直接跑 `.ts`，迁移只需改 `describe/it` 的 import 来源 |
| 端到端 | **Playwright**（替代自研 CDP helper） | 顺带获得截图与视觉回归能力 |
| 开发态 | `node --watch src/bin/simplex-hub.ts` + `vite dev` | 保持"改完即跑"，并首次获得 HMR |
| 生产态 | `node bin/simplex-hub.ts` | 与开发同一条路径：产物就是源码 |

**一个前置建议**：审计过程中实测复现了若干**后端 P0 缺陷，其中一个是远程未认证的进程崩溃**（§1.1）。这些与前端重构无关，但会让 hub 在重写期间随时挂掉。建议先做一个**独立的纯修复提交（P0 加固）**，不涉及架构变更，与后续重构解耦。

---

## 1. 现状诊断

### 1.1 后端（`hub/src/`，~4300 行 JS）：资产与隐患

**这套后端是被低估的资产，问题不在架构。** 它已经有：

- 清晰的模块边界：`http/`（传输）、`panel/`（面板协议）、`worker/`（worker 协议适配）、`state/`（注册表/转录/持久化）、`launch/`（进程监管）。
- 一个真正版本化的面板协议：`PANEL_PROTOCOL = {name, version: 1}`，每条消息带 `v:1`，未知字段保留、未知类型忽略。
- **已经存在、但前端从未读取的 `capabilities`**（`src/hub.js:26-32`）：`worker-events`、`confirmations`、`supervisor`、`transcript-replay`、`snapshot-view`。这是现成的扩展接缝。
- 一道少见的防线：`test/protocol-drift.test.js` 解析 `core/docs/worker-protocol.md`，core 新增事件/信号/选项类别而 hub 不认识时直接失败。
- JSDoc 注释密度很高，TS 迁移时签名基本是现成的。

#### P0：进程崩溃缺陷（已实测复现，与前端无关）

| # | 缺陷 | 位置 | 实测结果 |
| --- | --- | --- | --- |
| **B1** | **远程未认证 DoS**：`new URL(req.url, 'http://' + req.headers.host)` 位于 try 之外，非法 `Host` 头抛 `TypeError` → 无人处理的 rejection → 进程退出 | `src/http/server.js:78`（`void handleRequest` 在 :74） | 本会话实测：`curl -H 'Host: foo bar' /api/meta` → `TypeError: Invalid URL`，hub **exit code 1**。该路径在鉴权之前，`/api/meta` 本就免鉴权 |
| **B2** | **面板 WebSocket 崩溃**：`void handleMessage(...)` 无 `.catch()`；`supervisor.start()` 的 `mkdirSync`/`writeFileSync` 未防护，I/O 失败 → 未处理 rejection → 进程退出 | `src/panel/api.js:617`；`src/launch/supervisor.js:193, 210` | 审计复现：`dataDir=/dev/null/...` 时 WS 发 `worker:start` → `ENOTDIR` 崩溃。**同一失败走 REST 却返回干净的 500**——两个传输层行为不一致 |
| **B3** | **日志流未监听 `error`**：`createWriteStream(logPath, {flags:'a'})` 无 `'error'` 监听，异步 open/write 失败（ENOSPC、EACCES、目录被轮转）→ uncaught exception | `src/launch/supervisor.js:233` | 审计隔离验证：不可打开的路径 → `Unhandled 'error' event`，exit 1。`src/state/transcript.js:59` 注册了同样的监听，此处是遗漏 |
| **B4** | **`targetPid()` 盲信 pidFile**：没有 `/proc` 启动时间校验，而 adopt 路径是 fail-closed 的 | `src/launch/supervisor.js:345-355` vs `:451`、`isSameProcess` `:68-73` | 陈旧的 pid 文件会让 `stop()`/`forceKill()` 向一个已被回收的无关 pid（`processGroup:true` 时是整个进程组）发信号 |

B1 与 B2 的共同根因是 `void someAsync()` 这种"发射后不管"的调用方式散落在请求路径上；B3 是错误监听遗漏。**这三条与前端重构完全正交，应当先修。**

#### 其他后端问题

| 问题 | 位置 | 说明 |
| --- | --- | --- |
| worker 动作的回执前端收不到 | `src/panel/api.js:518-525` | 回执里 `action` 是 `start\|stop\|restart\|force-kill`，而面板判断的是 `message.action === 'worker'`（`web/js/app.js:394`）——**该分支永远不会命中**，点 Start/Stop 后面板没有任何反馈。后端返回的 `result`（含 pid/how/forced）被整体丢弃 |
| 面板 WS 与 REST 的上限不一致 | `src/panel/api.js:55` vs `src/http/server.js` | 面板 WS 的 `maxPayload` 硬编码 4 MiB，而 REST 与 worker WS 用 `limits.maxMessageBytes`（32 MiB）。超限时是 ws 的 1009 close，**没有版本化的 `error` 消息** |
| `spec` 创建时不校验、原样持久化 | `src/panel/api.js:300-301, 491-492` | 只在 start 时才 `normalizeSpec`；且 `normalizeSpec` 不校验 `env` 的值类型与 `extraArgs` 的元素类型（`src/launch/spec.js:63-64`），而这两者直达 `spawn` |
| 配置未知键被静默忽略 | `src/config.js:255-346` | 拼错的配置键不报错，只是不生效 |
| 畸形百分号编码返回 500 | `src/http/router.js:45`、`src/http/static.js:37` | `/api/sessions/%ZZ` → `500 internal_error` 并把原始 `URIError` 文本回给客户端，应为 400 |
| supersede 后上报虚假断开 | `src/worker/connection.js:285, 314-317` | 新连接替换旧连接后，被替换者的 close 回调仍无条件上报 `connected:false`，面板显示的连接状态会跳一下 |
| 日志写入无背压处理 | `src/launch/supervisor.js:270` | `logStream.write()` 未处理 `drain` |
| 转录内存被低估约 2× | `src/protocol/events.js:142,145` | `envelope.raw` 为每个文档保留第二份副本；`bytes` 数的是 UTF-16 单元而非字节，于是 `limits.transcriptBytes` 实际保护不到 |
| 模块反向依赖与重复 | `src/config.js:15`、`src/panel/api.js:21`、`src/worker/confirmation.js:32` | 配置层导入 mock 固件；传输层为一次 `join()` 导入启动器的路径策略；确认路由为取一个关闭帧字符串工具而拖入整个连接模块。另有 `parseListen`（`bin:49-60`）与 `parseAddress`（`mock/provider.js:33-40`）两份 host:port 解析 |
| 死代码 | `src/state/registry.js:307-324`、`src/panel/api.js:193-195, 308, 689` | `SessionRegistry.ensure/require`、`NotFoundError`、`authorized()`、`void body;` 与末尾 re-export 均无调用者 |
| 面板协议没有 drift 测试 | 无 | worker 协议有（`test/protocol-drift.test.js`），面板协议没有；`hub-protocol.md` 与实现存在漂移风险 |

> 模块图本身是**无环的**，`Session.describe()` 已经是面板协议的事实契约。这两点让 TS 化比看起来便宜。

#### P1：协议扩展性的真实缺口

这四条决定了"预留扩展空间"要补什么：

| 缺口 | 位置 | 影响 |
| --- | --- | --- |
| **版本号有三份独立字面量** | `src/hub.js:23`、`src/panel/api.js:26`、`web/js/api.js:12` | `hub.js` 从不导入 `panel/api.js` 的常量，所以 `protocol.version` 与线上 `v` 可以静默漂移；唯一相关的测试只断言两者都等于 1 |
| **没有 transcription epoch / 代际号** | `src/state/transcript.js:36` | `hub_sequence` 每个 hub 进程从 1 重新计数。hub 重启前取得的 replay cursor 在重启后会**静默返回空转录，且没有任何信号**——这是 A2 的第二个根因，且比 A2 本身更难发现 |
| **没有客户端 hello / 版本与能力协商** | `welcome` 是单向的 | 客户端无法询问"你支持 X 吗"，也无法声明"我是 v2 的面板" |
| **未知消息类型被静默丢弃** | `src/panel/api.js:596-598` | 只写一条 debug 日志、不回任何东西。较新的面板**无法区分"旧 hub 忽略了我的新命令"和"已处理"** |
| `capabilities` 是静态模块常量，且按引用返回 | `src/hub.js:26-32, 84` | 不随配置变化（`launcher.config: 'launcher'` 时仍宣告 `supervisor`），不是按实例构造；多个客户端共享同一个数组对象，测试中修改它会污染后续响应 |
| 其他缺失的接缝 | — | 无 WebSocket 子协议协商（`handleUpgrade` 没有 `handleProtocols`，`src/panel/api.js:661`）；REST 路径无版本前缀；`meta().worker_protocol` 是**文档路径**而非版本号（`src/hub.js:83`），hub↔worker 兼容性仅靠解析文档的 drift 测试保障 |

**同时要保住的三条接缝**（它们让 C++ 侧无需改动）：worker 路由按路径隔离（`/agent/:id/events`、`/agent/:id/confirm`）且面板消息永不触达 worker；worker 只消费两个 URL 加一份生成的配置文档；worker 事件在 hub 里**原样转发而不重塑**。只要这三条不变，浏览器协议的扩展就不需要动 C++。

### 1.2 前端（`hub/web/`，~4200 行）：手写 DOM 的结构性代价

`app.js` 1545 行 + `render.js` 1154 行，本质是在手搓一个没有 diff 能力的视图层。**下面 A 组三条不是审美问题，是"面板在常见场景下不可用"。**

#### A. 灾难级：三个功能性缺陷

**A1 · 未选中会话的审批提示无法回答（最严重）** ✅ **P2 已修（协议侧）**

面板一次只订阅一个会话（`app.js:446-457`：切会话时 `unsubscribe` 旧的、`subscribe` 新的），而 hub 的审批提示只广播给**该会话的订阅者**（`src/panel/api.js:130-135` `broadcastToSession`）。于是：

- 用户在会话 B 时，会话 A 的 worker 请求确认 → 面板**收不到** `confirmation` 消息。
- 恢复路径也是断的：`subscribed` 不携带提示列表（`src/panel/api.js:464-470`），而 `applySubscribed` 检查的是不存在的顶层 `message.confirmations`（`state.js:352`）——提示列表其实在 `message.session.confirmations` 里（`src/state/registry.js:273`，由 `Session.describe()` 产生）。
- 结果：操作者只看到侧栏一个 `1 confirmation(s)` 计数徽章（`app.js:626-627`），**没有任何办法打开并批准它**，直到 worker 自己的截止时间把它自动拒绝。

**这是"交互 bug"里代价最高的一个：它会静默地让工具调用失败。**

**修法**：`confirmation` 改为广播给**所有**已连接客户端，不再限于订阅者（`onPrompt` / `onPromptSettled`）。理由是审批是唯一"错过就失败"的消息，而同一 socket 上的每个客户端本来就共享同一把面板 token，扩大受众不授予任何新权限。这一处改动让**旧面板也立刻受益**——它收到 `confirmation` 就无条件弹窗，所以无需改前端。

回归测试 `panel-api.test.js` 的 "delivers a confirmation to a client watching a different session" 验证了"看得见"和"答得了"两半；还原成订阅范围后该测试失败，失败信息里能看到未订阅客户端收到的三帧（`welcome`、`subscribed`、以及带 prompt 的 `session`）**唯独没有 `confirmation`**——即缺陷的确切形状。

协议里同时把这条语义写清楚了：`hub-protocol.md` 明说 `confirmation` 是"仅订阅者"规则的**唯一例外**，并新增能力项 `global-confirmations` 供客户端检查。

**A2 · 每次面板 socket 重连都会清空可见转录**

`selectSession` 订阅时带 `since: lastSeq`（`app.js:456`），hub 只回增量（`src/panel/api.js:466` → `transcript.js:74-77` 的 `hub_sequence > since`），而客户端是**替换**而非合并（`state.js:150-152` `st.items = items;`）。重连（网络抖动、hub 重启、任何 socket close 后的 `welcome` → 强制重新订阅）之后，时间线变成"暂无事件"；同时 `latestEvents`/`runActive`/`lastRunId`/`gaps` 被一并重置（`state.js:155-159`），状态面板和输入区提示也跟着回退。

**A3 · Inspector 切换过的 tab 内容不会消失，只会一直往下堆**

`.ipane { display: flex }`（`app.css:476`）是作者样式，而 `.modal-root[hidden]`、`.overlay[hidden]` 都专门为重写了 `display` 的元素补了规则，**唯独 `.ipane` 没有**。作者样式表优先于 UA 样式表，所以 `pane.hidden = !active`（`app.js:1025`）根本不生效。

表现是**渐进的**，所以第一眼看不出来：`renderInspector()` 只渲染当前 tab 的面板，其余保持空 section，而空 section 没有高度——初次打开时右栏只有 Status 是可见的。但操作者每点一个 tab，那个面板就渲染出内容并**永久留在页面上**。

实测（`?session=demo`，依次点过 Options / Process / Logs / Snapshot 之后）：

| 面板 | `hidden` 属性 | 计算 `display` | 高度 |
| --- | --- | --- | --- |
| process | `true` | `flex` | **543px** |

也就是说，一个被明确标记为隐藏的面板仍占据半屏。截图里 tab 高亮停在 Snapshot，而右栏从上到下依次是 Status 的表、Options 的 "No options yet"、Process 的 "state running / pid 15227 / …"，Snapshot 自己的内容被挤到视野之外。

> 这直接解释了"所有的图标和按钮都堆在一起，错综复杂"的体感——而且解释了为什么它看起来像"用久了才变乱"。

同一个 bug 也打在确认横幅上：`.confirm-banner { display: flex }`（`app.css:224`）覆盖 `ui.confirmBanner.hidden = true`（`app.js:1281`），所以那个提示条永远占着位置。

> 修复时的坑（务必注意）：`.center` 声明了 5 行 grid（`app.css:63-68`），确认横幅占据第 2 行。天真地隐藏横幅会让 `#pane-chat` 落进 `auto` 行、把 `1fr` 让给输入区。

#### B. 输出呈现

1. **完全没有 markdown 渲染**。全仓库搜索 `markdown|marked|remark|highlight` 只命中一句注释和 `readable.md` 的提示文案。模型回复走 `renderContentValue()` → `<div class="content-text">` + `white-space: pre-wrap`（`render.js:236`），即纯文本。标题、列表、表格、代码块全部塌成一段。
2. **没有代码高亮、没有 diff、没有复制按钮**。工具参数一律 `JSON.stringify(…, null, 2)`（`render.js:317`），`run_command` 显示为 `{"command": "..."}` 而不是一条可读的命令。`readable.md` 也只是丢进 `<pre>`（`render.js:915`）。
3. **协议内部事件与对话内容等权渲染**：`input_admitted`、`input_committed`、`run_started`、`persisted`、`run_finished` 各自一张卡或 chip（`render.js:643-652`）。真实对话被埋在协议流水账里。
4. **`rawToggle` 几乎挂在每张卡片上**（`render.js:529, 550, 566, 590`），调试信息进入阅读路径。
5. **客户端不做截断**，只有 `max-height: 320px` 的视觉裁剪（`app.css:374-375`）。
6. **无法流式**：worker 协议只有完整 `model_response`（`src/protocol/events.js:30`），也没有"生成中"的状态指示。
7. **ID 一律截断到 8 字符且无法复制**（`render.js:110`）。

#### C. 信息架构

8. **10 个平铺的 peer 按钮**（`index.html:45-59`）：Start / Stop / Restart / Force kill / Status / Options / Cancel / Shutdown / Delete / Inspector，同尺寸同权重。三组语义（进程生命周期、控制信号、破坏性操作）零分组。
9. **破坏性操作权重倒挂**：`Force kill` 是红底（`index.html:49`），`Delete` 只是 ghost（`index.html:55`）。
10. **"Status"/"Options" 是信号伪装成视图**（`app.js:1086, 1140`），在按钮栏里与真正的进程操作并列。
11. **按钮可用性与真实状态脱节**：`renderHeader()` 里统一 `disabled = !session`（`app.js:727-730`），无进程时 `Stop` 可点、运行中 `Delete` 也可点。
12. **composer 把所有控件塞进一行**（`index.html:76-91`）：Send、Continue、image URL 输入框、Add ref、confirm 下拉、hint 文本，靠 `flex-wrap` 兜底。
13. **Inspector 默认展开占 320px**，Status 面板是 21 行 `dl.kv` 键值对（`app.js:1053-1080`）——转储对象而非呈现状态。
14. 侧栏 **8px 圆点 + `title` 属性** 表达状态（`app.css:123-134`），含义只能靠 hover 猜。

#### D. 其他交互缺陷

15. **`confirmation.mode` 跨会话泄漏（安全问题）**：`buildOptions()` 读全局 DOM 控件 `ui.confirmMode.value`（`app.js:938`），`renderSelection()` 从不重置它。在会话 A 选 `approve` 后切到会话 B，B **静默继承 `approve`**——自动批准该会话所有需要确认的工具调用，不再弹窗。按 `hub-protocol.md:233-235`，`approve` 等价于"批准每一个需要确认的工具调用"。
16. **审批弹窗的初始焦点在 "hide" 上**：`openModal` 聚焦 `focusables(element)[0]`（`app.js:141-143`），而 DOM 顺序里 head（含 hide 按钮）在 body/foot 之前（`render.js:1024-1029`）。打开弹窗后按 Enter = 静默隐藏。
17. **最小化的弹窗会自己弹回来**：`openConfirmationModal` 无条件 `hiddenConfirmations.delete(...)`（`app.js:1224`），而 `restoreOpenConfirmations()` 在每次 `renderSelection()` 时被调用（`app.js:1323`）。hide 掉、切走、切回来 → 它又出现。
18. **Approve/Deny 决策失败后永久禁用**：点击即 `disabled = true`（`render.js:955-956, 966-968`），失败路径只 toast（`app.js:1266-1269`），弹窗卡死无法重试。
19. **输入框乐观清空、无回滚**：`app.js:971-972` 在 `send()` 返回 true 时立即清空；hub 随后回 `input_not_sent`（`api.js:530-535`）时，用户输入的消息**已经丢了**。
20. **日志面板每次 Refresh 内容翻倍**：hub 回的是完整 tail（`api.js:583`），客户端却做 `concat`（`state.js:462`）。
21. **未保存的选项选择会被冲掉**：任何 `status`/`ready`/`options` 事件都会触发 `renderInspector()` → `clearNode` → 重建，并把 `select.value` 重置为当前值（`app.js:548-550 → 1043`，`render.js:775`）。
22. **DOM 无限增长**：store 裁剪到 2000 条（`state.js:138`），但时间线只 append、从不删除（`app.js:544, 799-833`）；Raw tab 每次全量重渲染每个 envelope 的 pretty-printed JSON（`render.js:688`，`app.js:852-867`）。长时间运行必然卡死。
23. **会话列表竞态**：`setSessions` 会删除不在 payload 里的 id（`state.js:303-305`），而"刷新"同时发 WS `list_sessions` 和 REST `sessions`（`app.js:1458-1463`），两个响应互相覆盖；共 4 处调用点（`app.js:239-240, 262-269, 411-415, 1458-1463`）。
24. **滚动被强制拉到底**：`renderTimelineFull()` 无条件 `stickScroll(true)`（`app.js:795`），挂在 `subscribed`/`snapshot` 上——向上翻阅历史时点一次 Status 就被拽回底部。
25. **`Load snapshot` 竞态**：`renderSnapshotPane()` 是 async（`app.js:1185-1215`），await 回来不校验会话是否已切换。
26. **`?session=` 深链接只读不写**（`app.js:436`），重连后失效。
27. **危险操作依赖 `window.confirm()`**（`app.js:1347, 1367`），与自绘 modal 体系不一致；且 force-kill 的文案无条件声称会杀进程组，**忽略了 `force_kill_process_group` 配置**（`hub.js:95`，前端从未读取）。
28. **死代码 / 死协议面**：从不发 `ping`（只处理 `pong`，`app.js:376`）；从不请求 `status_snapshot`（所以 `applySnapshot` `state.js:469-478` 不可达）；`rest.events/logs/worker/session`（`api.js:191-200`）从未调用——因此**没有任何 HTTP 回退路径**去恢复被 A2 清空的转录。
29. **1 秒倒计时 ticker 永不清理**（`app.js:194-209`）。

#### E. 可访问性与观感

30. **字号过小**：body 13px，`btn-sm` 11px，`mini-badge`/`cost`/`chip-meta` 10px（`app.css:35, 85, 115, 419）；按钮内边距仅 3×6px（`app.css:85`）。
31. **主题按钮的文字是"目标主题"**（`app.js:84`）——按钮上写着 `light`，无法判断是状态还是动作。
32. **用字符当图标**：`summary::before { content: "▸" }`（`app.css:328`）、`×` 关闭（`app.js:99`）。
33. **零 `@media`**：固定 `260px / 1fr / 320px` + `overflow: hidden` 的 100vh 壳（`app.css:30-52`），窄屏直接不可用。
34. **无键盘快捷键**、无命令面板、无搜索/过滤/复制/导出/永久链接。
35. **ARIA 不完整**：tab 无方向键导航与 `aria-controls`；转录无 `aria-live`（`index.html:67`）；连接徽章无 `role="status"`（`index.html:20`）；新建会话弹窗无 `role`/`aria-modal`（`app.js:1386`）；token 遮罩在焦点陷阱与 Escape 处理之外（`app.js:213-217`）；多个 `aria-modal` 对话框可共存，而陷阱横跨整个栈（`app.js:161-178`）。

#### F. 结构性根因

`app.js` 同时承担：DOM 引用收集、事件接线、store 变更分发、局部增量更新、全量重建、modal 栈、toast、倒计时 ticker、主题、认证。`render.js` 是一个手写的、没有 diff 能力的视图层。**任何新功能都要在这 2700 行里找到正确的插入点**——这才是"交互感生硬"的结构性来源。

#### G. 明确**不是**缺陷（重构时不要"修"坏）

- **没有任何 `innerHTML`**：不可信输出一律 `textContent`（`render.js:6`，并由 `test/panel-assets.test.js:112` 强制）。这是好设计，必须保留其语义。
- **监听器没有泄漏**：WS/DOM 监听器只在 `init()` 里接一次，持久节点上无 per-render 泄漏。
- **请求结果不从 `send()` 成功推断**：`sent` ≠ `executed` 这条纪律贯穿代码与 UI 文案，必须保留。

---

## 2. 技术选型

### 2.1 前端框架：React 19 + TypeScript + Vite

| 维度 | **React 19** | Vue 3.5 | Svelte 5 | SolidJS |
| --- | --- | --- | --- | --- |
| markdown / 高亮 / diff 生态 | 最全 | 全 | 偏少 | 少 |
| 无头可访问组件 | 最成熟（Radix） | 有 | 少 | 很少 |
| TypeScript 体验 | 最好 | 好 | 好 | 好 |
| 高频更新性能 | 中（需 memo + 窗口化） | 好 | 好 | 最好 |
| 运行时体积（gzip） | ~45KB | ~35KB | ~10KB | ~7KB |
| 生成式编码准确率 | 最高 | 高 | 中 | 低 |

**理由**：这是跑在 `127.0.0.1` 的本机工具，**体积和理论性能都不是约束，可维护性与生态才是**。面板需要 markdown、语法高亮、无头弹窗/菜单、长列表窗口化——React 在每一项上都有维护最好的选择。

**对"React 重渲染慢"的回应**：不靠框架原语，靠结构——对话块 `memo`、按 run 窗口化（默认只展开最近 N 轮，更早的折叠为摘要行）、日志面板独立订阅。**先做窗口化，不做虚拟列表**：虚拟列表与可变高度的 markdown 内容需要动态测量，复杂度不划算；实测不足再加 `@tanstack/react-virtual`。

**备选**：若偏好模板式写法，Vue 3.5 + `<script setup>` 是唯一无损替代（Pinia/Radix Vue 可平移）。

### 2.2 样式：Tailwind CSS v4 + Radix Primitives

- **Tailwind v4**：CSS-first 配置，token 写成 `@theme` 变量——与现有 `--bg/--text/--accent` 是同一思路的工业化版本，产物只含用到的类。
- **Radix Primitives**：现在的 `trapFocus()`/`onDocumentKeydown()`（`app.js:161-190`）是手写焦点陷阱，正是缺陷 A1/D16/D17/D18 的来源。交给 Radix 后，这一整类 bug 消失。
- **备选**：CSS Modules + 同一套 token（见 §8 待确认项 2）。

### 2.3 状态：Zustand

现有 `state.js` 的心智模型（一个 store + 每会话一个 view + 变更事件）方向是对的，只是消费端是手写 DOM。Zustand 保留它：

- selector 订阅 → 只有变化的组件重渲染（直接消除缺陷 D22/D23）。
- 可在 React 之外读写（WebSocket handler 里 `useStore.getState().applyEvent(...)`），socket 生命周期不必塞进组件。
- 每会话一个 view、`TRANSCRIPT_CAP = 2000` 等上限原样保留。
- **两处语义必须修正**：`applySubscribed` 改为**按 `hub_sequence` 去重合并**（修 A2），`setSessions` 改为**不做破坏性删除**（修 D23）。

不引入 TanStack Query：REST 只用于 `meta`/`sessions`/`snapshot`/`logs` 这几个一次性请求，主数据流是 WebSocket。

### 2.4 Markdown 与代码：react-markdown + remark-gfm + rehype-highlight

**这是回应 `test/panel-assets.test.js:112`「never writes panel markup as HTML」的关键**：`react-markdown` 生成 React 元素树，**不经过 `innerHTML`**，默认也不解析内联 HTML（不装 `rehype-raw`）。所以"渲染 markdown"与"不写 HTML"不冲突——那条安全测试的语义可以原样保留，只需把扫描范围改到 `web/src/**`，并把禁令收敛为 `dangerouslySetInnerHTML` / `innerHTML` / `insertAdjacentHTML` / `eval` / `new Function`。

- `remark-gfm`：表格、任务列表、删除线、自动链接。
- `rehype-highlight`（highlight.js 常用语言子集）：代码块高亮；语言标签 + 复制按钮通过自定义 `components.code` 渲染器加上。
- 链接协议白名单 + `rel="noreferrer noopener"`（现在 `render.js:245-248` 只对 `http(s)` 建 `<a>`，这个判断保留）。
- 升级点：`@shikijs/rehype` 可替换，组件层无感。

### 2.5 后端 TypeScript 化：只有一条轨道

| 场景 | 命令 | 说明 |
| --- | --- | --- |
| 开发 | `node --watch bin/simplex-hub.ts` | Node 原生 type stripping，**零构建** |
| 类型检查 | `npm run typecheck` | `tsc --noEmit`，CI 必跑 |
| 生产 / Docker | `node bin/simplex-hub.ts` | 与开发完全同一条路径：产物就是源码 |

**决策已定（P1 后）**：`engines` 从 `>=20.11` 提到 **`>=22.18`**，后端因此**不需要构建产物**——不再有 `dist/`，也没有"开发跑源码、生产跑编译产物"的双轨。原方案里的双轨是为保留 Node 20.11 设计的，而 P1 的实测证明那个目标与"共享 TS 契约"不可兼得（详见 §9）。`tsc` 的角色因此收窄为**纯检查器**：它从不产出文件，`noEmit` 常开。

代价与约束（写进 `tsconfig` 与编码约定）：Node 原生跑 TS **不能用 `enum`、`namespace`、构造函数参数属性**（用 `as const` 对象 + 联合类型替代，这本来就是更好的写法）；相对导入需带扩展名。CI 矩阵随之变为 `['22.18', '24']`。

运行时依赖仍然只有 `ws` 一个。

**关于迁移成本的准确评估**（我先前高估了现成类型的可用度，此处修正）：仓库里**没有 `tsconfig.json`/`jsconfig.json`，也没有一个 `@typedef`**；JSDoc 绝大多数是 `{object}` 这种无信息形状，真正能用的只有约 10 处内联形状字面量和那些 `const` 词表。所以类型要**新写**，而不是"把注释变成签名"。好消息是模块图无环、`Session.describe()` 已经是面板协议的事实契约，边界清楚。

| 难度 | 模块 | 说明 |
| --- | --- | --- |
| **hard**（4） | `panel/api.js`、`worker/connection.js`、`worker/confirmation.js`、`launch/supervisor.js` | 分别是：路由键是字符串 + 13 分支无类型 switch（**全仓库最高价值的类型化**）；ws 的 `RawData` 联合 + `bigint\|null` 序列号算术；可空 prompt 的长异步状态机 + `Promise.race` 返回 `object\|null`；`ProcessRecord` 状态机 + 条件赋值的 `monitor` + 不可信的在盘记录 + `'SIGTERM'` 裸字符串 |
| **moderate**（~10） | `bin`、`hub.js`、`config.js`、`http/server.js`、`protocol/events.js`、`state/registry.js`、`state/persist.js`、`launch/spec.js`、`launch/config-render.js`、`mock/provider.js` | 主要是动态拼装的配置对象、`any` 返回值、不可信 JSON 需要 `unknown` + 解析函数 |
| **trivial/easy**（~9） | `log.js`、`router.js`、`static.js`、`auth.js`、`session-id.js`、`transcript.js`、`util/ring.js`、`simplex-worker.js`、`command.js`、`launcher.js` | 纯逻辑或小接口；`RingBuffer<T>` 是泛型化的最佳候选 |

**类型能抓到的真实 bug**（不是理论收益）：`panel/api.js:520` vs `web/js/app.js:394` 的 `action` 不匹配；`supervisor.js:485` 的 `record.monitor` 可能为 undefined；三份重复的版本号字面量；`sequence` 的 `number|string` 联合流向前端；未校验的 `hub.json`/`spec` 抵达 `adopt`/`spawn`。

### 2.6 测试

| 层 | 现在 | 目标 |
| --- | --- | --- |
| 后端单元/集成 | `node:test` + `node:assert`，15 个文件共 ~3056 行 | **保留 `node:test`**（见下方说明），断言继续用 `node:assert/strict` |
| 面板资产完整性 | `test/panel-assets.test.js` | 保留并重写：扫描 `web/src/**`，契约升级为"类型检查 + 构建成功 + 无 HTML 注入" |
| 协议漂移 | 仅 worker 协议 | **扩展到面板协议**：比对 `hub-protocol.md` 的消息表与 `shared/protocol.ts` 的联合类型 |
| 面板端到端 | 自研 CDP + headless Chrome | **Playwright**：可截图、可视觉回归、自带浏览器管理 |
| 组件 | 无 | Vitest + jsdom，只覆盖需要 DOM 的组件测试 |

**修正（P1 实施后的结论）**：原计划把后端测试也迁到 Vitest，理由是"直接跑 `.ts`"。实际验证后发现 **`node --test` 本身就能直接跑 `.ts`**（Node 原生 type stripping，P1 已实测 `.js` 导入 `.ts` 成功），所以迁移 196 个已经稳定运行的测试没有收益，只有风险：Vitest 的 worker 模型对这些"起真实进程、开真实 socket"的集成测试并不天然更合适，而且 Vitest 5 要求 Node ≥22.12，会在 CI 的 floor job 上引入额外约束。**因此后端留在 `node:test`，Vitest 只用于将来需要 jsdom 的 React 组件测试。**

**P4 追加**：连"将来"也未必到来。P4 把前端 store 写成 **vanilla Zustand**（`zustand/vanilla`，React 绑定单独放在 `web/src/state/usePanel.ts`），于是 A2/D19/D20/D23 这些规则用 `node --test` 就测得了，不需要 jsdom；组件行为则由 Playwright 在**真实浏览器**里验证，比 jsdom 更真。所以 `vitest` 目前**仍是一个没有任何脚本使用的 devDependency**。留着它的理由是"下一个真正需要隔离 DOM 的组件测试不必重新决策"，但这是明确的取舍而不是疏忽——若确认不需要，删掉它只是 `package.json` 的一行。

现有测试的**长处**值得保留：worker 协议 drift（解析 `core/docs/worker-protocol.md`）、确认身份判定（fail-closed）、pid 复用的 fail-closed 采纳、协议优先停止、重放游标语义、未知事件容错、四条鉴权边界、静态路径封闭、"不注入 HTML"。这些不变量在重构中一条都不能丢。

**当前的空白**（重构要顺带补上）：`bin/` 的 CLI 在 P0 前完全无测试（**两条崩溃路径都在它的可达范围内**）；面板协议没有 drift 测试；并发 start/restart；采纳监控的清理；关闭后转录重开；日志流错误路径；pid 文件陈旧；面板 4 MiB 边界；`hub.stop()` 重入；跨 hub 重启的重放。

> 附注：本机 Chrome 因缺 `libnss3`/`libnspr4` 无法启动，现有 `test/e2e/panel.test.js` 在这种环境下会静默跳过——P1 实测确认了这一点。Playwright 自带浏览器可消除这个盲区，但它下载的 chromium **同样缺这几个系统库**，需要 `npx playwright install --with-deps chromium`（要 root）。在拿到 root 之前，本机跑 Playwright 需要 `LD_LIBRARY_PATH` 指向手工解包的库。

---

## 3. 目标架构

### 3.1 目录

```
hub/
  package.json  tsconfig.json  vite.config.ts
  shared/                  # ★ 协议契约，hub 与 panel 共用，无运行时依赖
    protocol.ts            #   消息联合类型、实体接口、Capability 联合
    guards.ts              #   运行时校验
    capabilities.ts        #   能力清单与版本
  src/                     # 后端（TS），结构照搬现有模块划分
    bin/simplex-hub.ts     hub.ts  config.ts  log.ts
    http/{server,router,static,auth}.ts
    panel/{api,handlers,registry}.ts
    protocol/{messages,events}.ts
    state/{registry,transcript,persist,session-id}.ts
    worker/{connection,confirmation}.ts
    launch/{launcher,supervisor,spec,command,config-render,simplex-worker}.ts
    mock/provider.ts  util/ring.ts
  web/
    index.html
    src/
      main.tsx
      app/{App,Layout}.tsx
      features/{sessions,transcript,composer,inspector,confirmations}/
      store/               # Zustand slices
      lib/                 # socket、REST、格式化
      styles/              # tokens.css + tailwind 入口
    dist/                  # 构建产物（gitignore）
  test/{unit,e2e}/
  docs/
```

`web/js/*.js` 与 `web/css/app.css` 整体退役；`web/index.html` 改为 Vite 入口。

### 3.2 构建与运行

```
npm start          # node dist/bin/simplex-hub.js（生产）
npm run dev        # 并发：hub（node --watch，原生跑 TS）+ vite dev server（HMR）
npm run build      # tsc（后端） + vite build（前端 → web/dist）
npm run typecheck  # tsc --noEmit（前后端）
npm test           # vitest run
npm run test:e2e   # playwright test
```

开发态 `vite dev` 代理 `/api` 与 `/panel/ws` 到 hub——比现在"改完手动刷新"更快，并首次提供模块热更新。

`src/http/static.js` 需三处调整：根目录指向 `web/dist`、SPA fallback（未知路径回 `index.html`）、带 hash 的资产长缓存而 `index.html` 用 `no-cache`。

Docker（`docker/Dockerfile.hub-test`）加前端构建步骤；`hub/web/dist` 与 `hub/node_modules` 一样进 `.dockerignore`。

### 3.3 必须显式处理的现存硬约束

这些是测试与服务器代码里**写死的假设**，重构会正面撞上，逐条列在这里以免遗漏：

| 约束 | 位置 | 处理方式 |
| --- | --- | --- |
| 静态根就是 `hubRoot/web`，**没有 SPA fallback**——未知深链接返回 404 纯文本 | `src/http/server.js:71`、`src/http/static.js:55-72` | ✅ 改指向 `web/dist`；**fallback 没有做，而且不该做**：面板是单页、无客户端路由，`?session=`/`?token=` 是查询参数，所以不存在的路径本来就该是 404。未构建时答 503 + 构建命令 |
| `index.html` 必须引用 `/css/app.css` 与 `/js/main.js`，且每个 `src\|href` 都要存在；`web/js/*.js` **至少 4 个顶层模块**且每个都要能 `node --check` | `test/panel-assets.test.js:56-99` | 测试重写为"构建成功 + 类型检查 + 入口存在"；`node --check` 那套随无构建模式一起退役 |
| CSS 必须是 `:root{` + `[data-theme="dark"]{` 两套 token，且 JS 要设置 `data-theme`；e2e 要求 `#theme-toggle` 之后 `dataset.theme` 恰好是 `light\|dark` | `test/panel-assets.test.js:101-110`、`test/e2e/panel.test.js:146-150` | 保留两套 token 与 `data-theme` 机制；选择器改为 `data-testid` |
| e2e 硬编码了整套 DOM 契约 | `test/e2e/panel.test.js:84, 87, 113, 117-123, 127, 130-143` | Playwright + `data-testid` 重写（计划内破坏） |
| `?session=` 与 `?token=` 两个查询键是契约 | `web/js/api.js:65-85`、`web/js/app.js:317-320`、`test/e2e/panel.test.js:79` | 必须保留 |
| 缓存策略：静态 `no-cache`、API `no-store`；**没有 ETag，没有 hash 资产方案** | `src/http/static.js:76`、`src/http/server.js:25` | ✅ `cacheControlFor()`：`-<hash>.js/.css/...` 一年 immutable，其余（尤其文档本身）no-cache |
| **完全没有安全响应头**（CSP / X-Frame-Options / X-Content-Type-Options / Referrer-Policy / CORS 一个都没有）；跨源防护只有面板 socket 的 Origin 检查，而**缺 Origin 头时直接跳过** | `src/panel/api.js:645-660` | ✅ `src/http/panel-headers.ts`：nosniff / Referrer-Policy / X-Frame-Options / COOP，加上 **CSP**——内联脚本的 hash 从构建产物里算，所以策略与文档不可能不一致。**Origin 缺失的分支不改**，理由见 §6.5 |
| 传输层字面量：`/panel/ws`、`/api` 前缀、按段数精确匹配的路由、`'METHOD /path'` 路由键格式 | `src/panel/api.js:631`、`src/http/server.js:80`、`src/http/router.js:39`、`src/hub.js:161-170` | 保持不变；新路由沿用同样的键格式 |

---

## 4. 协议扩展空间设计（重点）

### 4.1 先划清一条边界（必须诚实说明）

**真正的流式输出在 Worker 协议里不存在**：`core/docs/worker-protocol.md` 只定义了一次性的 `model_response`，没有 token 级增量事件。C++ 侧不改，hub 就**无法**提供真流式。

面板侧能做的：完整回复到达后的渐进呈现动画、工具执行期间的真实状态。**这不能伪装成流式**——否则用户以为模型在逐字输出，实际是等了几秒后一次性吐出。

预留方式：面板渲染层按「事件 → 对话块」映射设计，未来 worker 增加 `model_delta` 一类增量事件时，只需把增量追加到同一个块，**协议版本号不必跳变**。今天就可以在 `shared/protocol.ts` 的事件联合里把 `model_delta` 标为 `reserved`，并让渲染层走"未知事件 → 通用折叠卡片"的回退路径。

### 4.2 把已有的接缝真正用起来

下表在 P2 完成后重写过一遍：左列是当初的判断，右列是**实际落地的**，包括两处与计划不同的决定。

| 扩展机制 | 当初的现状 | 实际做法 |
| --- | --- | --- |
| 消息类型 | 两端各写一份字符串字面量（`src/panel/api.js` 与 `web/js/app.js` 各一个 switch） | ✅ `shared/protocol.ts` 定义 `PanelMessage` / `HubMessage` 两个 discriminated union（13 + 16 种），并导出 `PANEL_MESSAGE_TYPES` / `HUB_MESSAGE_TYPES` 供 drift 测试比对。**注意**：`.js` 消费端在 P3 之前拿不到类型检查，所以"编译期抓住 `action === 'worker'` 死分支"这件事要等 P3 才兑现——P2 兑现的是运行时 guard 与 drift 测试 |
| 能力协商 | `capabilities: string[]` 已下发，但前端零消费，且是静态常量 | ◐ 能力清单移到 `shared/`、加了 `Capability` 联合、meta 每次返回**独立副本**（此前按引用返回同一数组，一个调用方可以替所有人改掉它）。**但没有做成"从配置推导"**：实际检查后发现没有配置相关的能力——`supervisor` 的意思是"这个 hub 会启动并给 worker 进程发信号"，与哪个 launcher 渲染配置无关，而 launcher 的差异已经由 `launcher.owns_config` 单独报告。从配置推导这个列表会是对空集的抽象。✅ **P4 起前端真的消费它了**：`subscribe` 的游标是否带上取决于 `transcript-replay`（hub 没声明这条能力时，`since` 对它没有承诺过的含义，面板就从 0 要全量）|
| 能力版本 | 无 | ⏸ **未做**，见下方"推迟的两件事" |
| 审批的可见性语义 | **隐式**：只有订阅者收得到（A1 的根因） | ✅ `confirmation` 广播给所有客户端；`hub-protocol.md` 明写它是"仅订阅者"规则的唯一例外；新增能力项 `global-confirmations`。**这一处修复让旧面板立刻受益，无需改前端** |
| 重放语义 | `subscribed` 的 delta/full 语义未写明，客户端当成 full（A2 的根因） | ✅ 协议侧写明 `transcript` 是 `since` 之后的增量；P4 的 store **按 `hub_sequence` 去重合并**，回归测试与浏览器测试都还原验证过会咬住 |
| **转录代际（epoch）** | **完全缺失**：`hub_sequence` 每个 hub 进程从 1 重来（`transcript.js:36`），重启前的 cursor 会静默返回空转录 | ✅ `meta` / `welcome` / `subscribed` 都带 `transcript_epoch`（每进程一个 UUID）；新增能力项 `transcript-epoch`。**这是 A2 的第二个根因，且比 A2 更隐蔽**。✅ P4 在 `welcome` 与 `subscribed` 两处对账：epoch 变了就重置游标、保留已读历史、加一条说明并重新要全量；hub 太老不报 epoch 时，用 `latest` 倒退作为兜底信号 |
| **客户端 hello / 协商** | 无。`welcome` 是单向的，客户端无法声明自己是 v2，也无法询问"你支持 X 吗" | ⏸ **推迟**，见下方 |
| 未知消息类型的反馈 | 静默丢弃，只写 debug 日志（`api.js:596-598`） | ✅ 保持"忽略"语义不变，但 `checkEnvelope` 把"未知类型"与"信封损坏"**分成两种结果**，调用方因此能分别对待；hub 侧仍是记 debug 并忽略 |
| 结构演进 | 未知字段保留 | ✅ 契约里可能缺席的字段标 `?`（如 `transcript_epoch`、`log_path`）；`WorkerEnvelope` 保留索引签名，因为 worker 事件本就允许任意字段 |
| 入站校验 | hub 端逐 case 手写 | ✅ `shared/guards.ts` 的信封校验两端共用；**只做信封**（解析、对象、版本、类型），消息体的校验留在 hub，因为那依赖 hub 状态而浏览器里没有 |
| Worker 事件透传 | 已 verbatim 转发 + `known`/`issues`/`raw` | ✅ **刻意保持不变**——这是最重要的一条：core 新增事件名时面板天然能收到，不需要协议升级 |
| 契约测试 | 只有 worker 协议的 drift 测试 | ✅ `test/panel-protocol-drift.test.js` 解析 `hub-protocol.md`，比对**三张表**：面板消息类型、hub 消息类型、错误码，外加能力清单与两条语义断言 |

#### 推迟的两件事（以及触发条件）

- **`hello` 协商与能力版本 `features`**。它们的价值是"客户端声明自己是 v2，hub 据此降级输出"。但现在只有一个版本，加了也不会被消费——那就是死代码。**触发条件**：出现第一个必须破坏兼容的改动（协议升到 v2）时，同时引入 `hello` 与 `features`，并用 WebSocket 子协议（`Sec-WebSocket-Protocol`）而不是消息体来声明版本，因为那在握手阶段就能拒绝，而不是先升级再报错。
- **未知类型的可选应答**（`unsupported_capability`）。同样取决于 `hello` 是否存在：没有协商，hub 无法知道对端是否承受得起一条新错误消息，静默忽略仍是更安全的默认。

这两条都不是被遗忘，而是被**明确排在触发条件之后**。

### 4.3 版本策略（写进文档与 `protocol.ts` 注释）

- **Additive（不升版本）**：新增消息类型、新增可选字段、新增枚举取值、新增能力项。
- **Breaking（升到 `v: 2`）**：删除/重命名字段、改变字段语义、可选改必填。升版本时 hub 需在一段时间内同时接受 v1 与 v2，并对 v1 客户端降级输出。
- 面板必须"**对新字段容错、对未知消息类型忽略**"；hub 必须"**对未知字段保留、对未知消息类型忽略**"。这两条现在已是行为，改造后用测试固定住。

### 4.4 建议补齐的协议能力（均 additive）

P2 落地后的状态：

1. **`transcript-epoch`** ✅ **已实现**——它原本排在后面，但它是 A2 的第二个根因（跨重启的重放静默返回空），不做的话前端重写时会再踩一次。已加进能力清单与文档。
2. **`global-confirmations`** ✅ **已实现**——A1 的修复本身就是一条协议语义，所以它值得一个能力名，让客户端能检查而不是假设。
3. **`event-page`**：`GET /api/sessions/:id/events` 已有 `since`/`limit`，但面板一次拿全量；加游标分页能力。P4 前端需要它时再做。
4. **`session-search`**：跨会话搜索转录（hub 已有 JSONL 文件，成本低）。未做。
5. **`transcript-delta`**：显式声明增量重放语义。P2 把语义写进了文档，但没有单列能力项——`transcript-replay` 已经表达了"支持按游标重放"，再拆一个只会让客户端多检查一次。
6. **`audit-log`**：审批决策的追加日志——现在是 toast，无留痕。**安全相关，建议早做**，但不在 P2 范围内。
7. **`multi-client`**：显式声明多面板协同。协议本就支持，且 A1 的修复让"多个面板同时接审批"成为现实——但现在还没有客户端**依赖**这个声明，所以暂时不加。
8. **worker 回传已准入输入的文本**（不在 hub 协议里，属于 core）。见 §6.1 的 N3：这是面板唯一**无法**在现有协议下做对的事——刷新之后，操作者看不到自己说过什么。**触发条件**：本次任务明确要求"不动 C++、不改 worker 协议"，所以推迟；一旦允许动 core，最小改动是在 `input_admitted` 的 `data` 里回带被准入的内容（而不是新增事件），因为该事件本就与那次输入一一对应。在那之前，前端只做一件事：把"这里本该有内容"如实写出来，而不是留白。

---

## 5. UI/UX 重设计方案

### 5.1 布局

保持三栏骨架（骨架没问题，问题在每栏的内容），改变权重与默认值：

```
┌──────────────┬────────────────────────────────────┬──────────────┐
│ 会话          │  会话头：名称 · 状态徽章 · 主操作    │  上下文抽屉   │
│  搜索框       │  ────────────────────────────────  │  （默认收起） │
│  ● demo 运行中│                                    │              │
│  ○ test  空闲 │  对话流（用户/助手/工具）            │  Run         │
│  ⚠ 待审批     │  助手消息 = markdown + 代码高亮      │  Process     │
│               │  工具调用 = 可折叠卡片 + 命令高亮     │  Logs        │
│               │  协议事件 = 默认收进"技术细节"        │  Snapshot    │
│               │  ────────────────────────────────  │              │
│               │  输入区：+ 附件 · 文本域 · 发送       │              │
└──────────────┴────────────────────────────────────┴──────────────┘
```

- **Inspector 默认收起**，需要时抽屉滑出（现在默认占 320px 且常驻）。用 React 条件渲染，A3 那类 `hidden` 失效问题在结构上不可能发生。
- **协议事件默认隐藏**，顶栏一个"显示技术细节"开关。

### 5.2 会话头：从 10 个平铺按钮到三个层次

| 层次 | 内容 | 呈现 |
| --- | --- | --- |
| 主操作 | Start ⇄ Stop（按状态二态切换） | 唯一的实心按钮 |
| 常用 | Status、Options | 图标按钮 + tooltip |
| 溢出 | Restart、Shutdown、Force kill、Delete | `⋯` 菜单；破坏性项红色 + 二次确认 |

- 按钮 `disabled` 反映**真实状态**（无进程时 Stop 灰、运行中 Delete 灰并说明原因），不再"有会话就全亮"。
- 危险操作用自绘对话框替换 `window.confirm()`，并**根据 `force_kill_process_group` 实际值**描述后果。
- 审批待处理时，会话行与顶部显示醒目的**全局待审批徽章**（直接修 A1 的可见性）。
- 命令面板（`⌘K`）：切换会话、启停、发信号、开关技术细节。

### 5.3 对话流

| 内容 | 呈现 |
| --- | --- |
| 用户消息 | 右侧气泡，窄栏，长文可折叠 |
| 助手回复 | **markdown**：标题、列表、表格、引用、GFM 任务列表；代码块带语言标签 + 复制按钮 + 高亮 |
| `reasoning` | 默认折叠的"思考"区块（现在是 `<details>`，保留思路，改进样式） |
| 工具调用 | 卡片：图标 + 工具名 + 状态（待确认/执行中/成功/失败）+ 耗时；`run_command` 用 shell 高亮显示命令本身而非 JSON |

> **P5 实测后的修正**：状态做全了（待确认 / 运行中 / ok / failed / 未执行 / 无结果上报，六态而非四态）；"耗时"**没有**做成工具耗时——worker 协议没有 per-tool start/finish 事件（`core/docs/worker-protocol.md` 明说不定义）。卡片显示的是两件可以负责的事：工具自己在输出里报的 `running_milliseconds`（标注为"工具自己报告的"），以及从提议信封到结果信封的墙钟（标注为 proposal → result）。图标留到 P7 的图标体系一起做。 |
| 工具结果 | stdout/stderr 分流；错误红色左边框；超长折叠 + "展开全部" |

> **P5 实测后的修正**："分流"在协议层面**不存在**——协议原文是"these are not separate transport streams"，进程工具只是把 stdout/stderr 渲染成同一段文本里的具名段落。所以做的是**按结构读那段文本**（`toolOutput.ts` 识别 `[[field]]: value` 与 `name (N bytes):` 两种段落），识别不了就原样显示，并保留"raw output"开关。而生产者自己的头文件写着这些标记"不是机器协议、不是安全边界"，所以这里只把它们当**显示**线索用，不从里面推导成功/失败/安全性。 |
| 协议事件 | 默认收起为细弱时间线；开关打开后与现状等价（保留 `raw` 折叠块） |
| run 分组 | 可折叠的"轮次"区块，摘要行显示模型/耗时/token；默认只展开最近 N 轮 |

> **P5 实测后的修正**：摘要行显示的是状态、回复数、工具数、交换数、墙钟、token、以及被折起的协议事件数——**没有模型**：`model_response` 和 `status` 对象都不含模型名，所以"这一轮是哪个模型答的"在协议里无从得知。模型是会话级的，留在会话头。默认展开最近 3 轮。 |

- **窗口化优先于虚拟列表**（理由见 §2.1）。
- **滚动行为修正**：仅当用户已在底部附近才自动跟随；提供"跳到最新"悬浮按钮。
- **转录合并而非替换**（修 A2），并在重连后显示一条不打断阅读的提示。

### 5.4 输入区

- 自适应高度文本域（现在固定 `rows="3"`）。
- 左下 `+` 添加 `external_ref`（图片 URL）；part chips 保留并美化。
- Enter 发送、Shift+Enter 换行、Esc 清空；**发送失败时保留草稿**（修 D19）。
- **`confirmation.mode` 改为 per-session 存储**（修 D15），移入 `⚙` 弹出层，当前值用带色徽章常驻显示——`approve` 状态尤其要显眼。
- 状态行：run active / socket 未连接 / 排队中。

### 5.5 设计系统

- **Token**：颜色（OKLCH，light/dark 两套）、间距（4/8/12/16/24）、字号（12/13/14/16/20/24）、圆角、阴影、动效时长与缓动。现有 `--bg/--bg-panel/--text/--accent` 映射过去。
- **字号**：正文 14px（现在 13px），最小 12px（现在大量 10px）。
- **组件**：Button（primary/secondary/ghost/danger × sm/md）、IconButton、Badge、Card、Dialog、Drawer、Menu、Tooltip、Tabs、Input/Select/Textarea、Toast、EmptyState、Skeleton。
- **动效**：进入 fade + 轻微位移，120–180ms；hover/active 有明确反馈（现在 hover 只改边框色）；尊重 `prefers-reduced-motion`。
- **可访问性**：焦点可见且不丢（现在切会话/重渲染会丢）、Tab 顺序合理、Radix 提供正确的 menu/dialog 语义、状态不只靠颜色、日志与图表有文本替代。

---

## 6. 迁移路线

分阶段，**每阶段结束时测试全绿、hub 可用**。

| 阶段 | 内容 | 验收 |
| --- | --- | --- |
| **P0 加固** ✅ 已完成 | B1（HTTP + upgrade 两处畸形 authority）、B2（面板 WS 的 `handleMessage` + supervisor 的 `mkdir`/`writeFile`）、B3（日志流 `error` 监听）、B4（`targetPid` 的 `/proc` 校验）、畸形百分号编码改 400、`bin` 的 `void stop(signal)` 补 catch（并在半途失败时 `process.exit(1)`，否则会挂着继续占端口）、事件扇出**双层**保护、supersede 不再上报虚假断开、config 拒绝未知键、`spec` 在 REST 与 WS 两条创建路径都提前校验、adopt 监控的 `unref` 与 `finish` 清理 | **25 个回归测试**（`test/hardening.test.js`），全套 **196 个测试**加 2 个真实 worker 端到端通过；每条修复都还原验证过测试确实能咬住 |
| **P1 脚手架** ✅ 已完成 | `tsconfig.json`（后端，`allowJs` 让迁移可以逐个模块进行）+ `web/tsconfig.json`（前端，DOM lib）+ `vite.config.ts` + `playwright.config.ts`；`shared/protocol.ts` 落地并把三份版本字面量钉住；CI 拆出 `hub-panel` job（build + 浏览器测试，Node 24）并给 `hub-test` 加 typecheck；`.gitignore`/`.dockerignore` 加 `dist` | typecheck 通过**并验证过能抓到注入的类型错误**；面板构建产出 `web/dist`；Playwright 2/2 通过；Node 24 上 200 个测试全绿。（P1 刚完成时 Node 20.11 上还是 196 通过 0 失败——靠一个具名 skip；floor 随后按 §9 提到 22.18，该 skip 已删除。） |
| **P2 协议契约** ✅ 已完成 | `shared/protocol.ts` 扩成完整契约：13 种面板消息 + 16 种 hub 消息的联合类型、实体接口、错误码、能力清单；`shared/guards.ts` 提供两端共用的信封校验；hub 与面板的版本常量收敛到 shared（旧面板那份保留并加守卫，理由见下）；**A1 修复：审批广播给所有客户端**；新增 `transcript_epoch`；新增面板协议 drift 测试。**与计划的偏差**：能力清单没有做成"从配置推导"（没有配置相关的能力，见下）；可选的 `hello` 协商**推迟**，理由见下 | 222 个测试 + 2 个真实 worker 端到端通过；A1 与 epoch 各有回归测试，两者都还原验证过；浏览器端确认：看着 session A 时，session B 的审批弹窗确实会出现 |
| **P3 后端 TS 化** ✅ 已完成 | 全部 22 个模块 `.js` → `.ts`，按依赖图从叶子到根分批（每个提交只改 import 说明符，逐条核对过）；新增 `@types/ws`；`tsconfig` 收紧了 `allowJs`/`checkJs` | 每一步 `npm test` 222 全绿；真实 worker 的端到端在每次触及 supervisor 的批次后都跑；`src`/`bin`/`shared` 下已无 `.js` |
| **P4 前端骨架** ✅ 已完成 | Vite + React 壳：布局、Zustand store（从 `state.js` 平移并修 A2/D23）、socket/REST 客户端、会话列表、可显示事件的最小对话流 | 23 个 store 回归测试 + **6 个浏览器测试**（新增 `test/browser/stub-hub.mjs`，可被脚本化地 emit / confirm / restart）+ 真实 hub 与真实 worker 的 `npm run check:panel`。四条主修复（A2 合并、D23 合并、epoch 重置、D20 日志尾部）都**还原验证过测试会失败**，A2 在浏览器里也单独还原验证过。P4 期间新发现三个问题，见 §6.1 |
| **P5 对话流** ✅ 已完成 | markdown + GFM（react-markdown，不经 HTML）、代码高亮与复制、工具卡片（命令当命令读、结果按结构读）、run 分组与窗口化、技术细节开关、长消息与长输出折叠 | 20 个纯函数测试（`test/panel-transcript.test.js`）+ 9 个新浏览器测试（共 15 个）+ 真实 hub/worker 全流程。**§5.3 有三条按字面做不到**，改成如实呈现，见 §6.2；另外发现 P4 的移植丢掉了一处**旧面板本来知道的事**（N5） |
| **P6 交互重构** ✅ 已完成 | 会话头层次化、溢出菜单、危险确认对话框、composer、Inspector 抽屉、命令面板、per-session 确认模式 | **§1.2 的 D 组 15 项全部关闭**，逐条对应见下方 §6.2 表。10 个新的纯函数测试（`test/panel-palette.test.js`）+ 12 个新的浏览器测试（共 30 个）+ 真实 hub/worker 全流程无 console error |
| **P7 打磨** ✅ 已完成 | 语义 token 层（OKLCH，明暗两套）+ Tailwind 自带调色板整体关闭；light/dark/system 三态主题，首屏前置脚本消除闪白；单一图标模块（一个含义一个图标、三档尺寸、统一线宽）；动效 token + 单一 `prefers-reduced-motion` 收口；`md` 以下两侧栏改为抽屉；骨架屏与空状态；a11y 专项（对话框聚焦自身而非首个按钮、审批 live region、焦点环、状态不只靠颜色） | **键盘可完成全流程**（选择会话 → 启动 worker → 发送消息，全程只用 `page.keyboard`）；**390px 可用**（无横向滚动、抽屉可开可关、composer 可发）；对比度**实测**而非声称：浅色 17.8/17.8/6.5/4.8:1，深色 14.7/14.7/8.0/4.7:1。P7 发现两个类型检查与构建都看不见的缺陷，见 §6.4 |
| **P8 收尾** ✅ 已完成 | 删除 `web/js`、`web/css`、旧 `web/index.html`（新面板 `app.html` 更名 `index.html`，成为 Vite 默认入口）；`src/http/static.ts` 改服务 `web/dist`，未构建时回 **503 + 构建命令**；`test/e2e/panel.test.js` 与 CDP helper（`test/helpers/browser.js`）删除，Playwright 是唯一的浏览器路径；`protocol-constants` 去掉第三份版本字面量并改为结构性断言；`panel-assets` 按单面板重写；README/hub-protocol/worker-adapter 更新（含 15 处 `.js`→`.ts` 路径）；CI 增加「hub 服务自己构建出的面板」一步；Dockerfile 增加 `npm run build`；另外补上 hash 资产长缓存与安全响应头（含按文档内联脚本 hash 生成的 CSP，见 §6.5） | 文档与实现一致：`npm test` 297、`npx playwright test` 47、`npm run test:e2e` 2、三份 tsconfig 全清；真实 hub 验收 `npm run check:panel` **直接加载 hub 自己服务的面板**（不再经任何 preview 代理）全流程通过、console error 为零 |

P4 之前不做视觉改动；P4–P7 期间旧面板保持可用（Vite 产物与旧静态文件并存），P8 已删——迁移期间它一直有测试守着，所以整个重写过程中 hub 始终可用，而不是拿一个能用的面板去换一个半成品。

### 6.1 P4 期间新发现的三个问题

都不是计划里写过的，是写前端时才暴露出来的。

**N1 · `subscribed` 从来没有 `confirmations` 字段，而旧面板一直在读它。**
`web/js/state.js:352-356` 遍历 `message.confirmations` 来恢复审批弹窗，但 hub 的 `subscribed` 从来只发 `session / transcript / logs / latest / transcript_epoch`（`src/panel/api.ts:601-615`）。所以那段代码是死的：**刷新页面后，一个仍然开着的审批在界面上不存在**，直到某条后续帧碰巧提到它——而按 A1 修复前的语义，"某条后续帧"不会再来了，于是工具调用只能等到 deadline 被自动拒绝。
恰好 `SessionDescription.confirmations` 本来就是权威来源，所以 P4 直接改从会话描述里取（`welcome` 与 `subscribed` 两处）。这也是 A1 修复真正被消费的地方：现在**看着 session A，session B 的审批会出现在屏幕上并且答得了**，浏览器测试 `an approval for a session the panel is not watching still arrives (A1)` 覆盖了这条。

**N2 · hub 的面板 upgrade 会校验 `Origin` 与 `Host` 一致，而 Vite 代理的 `changeOrigin: true` 恰好会踩中它。**
`src/panel/api.ts:820-837` 拒绝 origin 与 host 不符的面板 upgrade——这是防跨站 WebSocket 劫持的正确做法（本机 loopback 上的 hub 不该被任意网页驱动）。但 `changeOrigin: true` 会把 `Host` 改写成目标地址，于是**每一次经代理的 upgrade 都长得和攻击一模一样**，hub 回 403，浏览器测试全挂在"connect 不上"。
修法不是放宽 hub，而是代理不改写 `Host`（`vite.config.ts` 的 `hubProxy`）。同时给 `stub-hub.mjs` 加了同一条校验：一个不检查 `Origin` 的桩服务会让这个错误只在真实 hub 上出现，那正是"测试通过、产品失败"的形状。

**N3 · 操作者自己发的消息，worker 协议不回传。**
`input_admitted` 与 `input_committed` 的 `data` 都是 `{}`（`core/docs/worker-protocol.md` 的事件表），模型侧的 `model_response` 也不含用户消息。所以**面板是这段话唯一存在的地方**：P4 因此加了一个 outbox 项，按面板自己生成的 `request_id` 与 `input_admitted` 对上；刷新之后它就没有了，重放里只剩一条"用户输入——worker 协议不报告其文本"的占位。这不是前端能修的，也不该假装能修。

**N4（小）· `tool_calls` 与 `model_response.invokes` 是同一批调用的两份表示。**
一次 `run_command` 会在时间线上出现两张卡。旧面板也是两张，是"输出乱"的一部分。**P5 已修**：按 call id 合并成一张卡，只按 id——因为两者**允许不同**（`tool_calls` 是 dispatch 前的提议，`model_response.invokes` 是消息的一部分），只出现在一边的调用仍然单独成卡而不是被丢掉。回归测试 `draws one card for a batch the response and the event both describe`（单元 + 浏览器各一条）。

### 6.2 P6：D 组缺陷逐条关闭

验收条件是"§1.2 的 D 组缺陷全部关闭"，所以逐条列出**关闭机制**，而不是只写"已修复"。
标 P4/P5 的表示在前面阶段已经关闭。

| # | 缺陷 | 关闭方式 |
| --- | --- | --- |
| 15 | `confirmation.mode` 跨会话泄漏（安全） | 模式存在 store 的 `confirmMode: Map<sessionId, mode>` 里，随每条消息的 `options.confirmation.mode` 发送；非默认值时**常驻徽章**显示。浏览器测试：会话 A 设 approve → 切到 B → B 仍是 ask，且发出的消息里带的是 `ask` |
| 16 | 审批弹窗初始焦点在 "hide" 上 | 弹窗以 `focus="none"` 打开（`onOpenAutoFocus` 阻止默认），**没有任何按钮被武装**。测试：打开后按 Enter，弹窗仍在，且 hub 没收到任何决策 |
| 17 | 最小化的弹窗会自己弹回来 | 关闭即"推迟"，记在 `deferred` 集合里，不会因重渲染重新打开；横幅保留、一键可回。测试：Later → 推一条事件（即一次重渲染）→ 仍然关着 → Review 能打开 |
| 18 | 决策失败后按钮永久禁用 | **决策按钮从不 disabled**：组件只在 hub 仍把 prompt 列为 open 时存在，所以点击只能是重发。测试：点两次 Approve，hub 收到两条决策 |
| 19 | 输入框乐观清空、无回滚 | ✅ P4：store 把文本交还 composer |
| 20 | 日志面板每次 Refresh 翻倍 | ✅ P4：`logs` 是环形缓冲的尾部，替换而非拼接 |
| 21 | 未保存的选项被状态事件冲掉 | ✅ 结构上：Inspector 是 React 条件渲染，选项（tab、快照开关）在 state 里，事件不再重建 DOM |
| 22 | DOM 无限增长 | ✅ P5：窗口化（只展开最近 3 轮）+ 有界转录 |
| 23 | 会话列表竞态 | ✅ P4：刷新列表只合并，`welcome` 才替换 |
| 24 | 滚动被强制拉到底 | ✅ P4/P5：只在读者本来就在底部时跟随 |
| 25 | `Load snapshot` 竞态 | 快照带 `sessionId`，store 丢弃不属于当前会话的回包。测试：延迟 1.5s 的请求 + 中途切会话 → 旧会话的状态不出现 |
| 26 | `?session=` 只读不写 | ✅ P4：选中即写回地址栏（命令面板测试顺带验证） |
| 27 | 危险操作依赖 `window.confirm()`，且 force-kill 文案无视配置 | 自绘对话框；文案**按 `force_kill_process_group` 实际值**分叉。测试：两种配置下文案不同，且整个过程浏览器原生 dialog 事件为零 |
| 28 | 死代码 / 死协议面 | `ping` 现在**会发**（命令面板的 "Ping the hub"，显示往返毫秒）；`status_snapshot` 已经是"reload transcript"的实现；`rest.events` 成为**socket 断开时的 HTTP 回退**（测试把 hub 停掉再 reload，证明走的是 REST）；没有读者的 `rest.session` 删掉了 |
| 29 | 1 秒倒计时 ticker 永不清理 | ✅ 结构上：新面板没有任何 `setInterval`；审批显示的是 deadline 时刻而不是倒计时 |

其中三条值得单独说，因为它们是**在修的过程中发现"修法本身又会变成同一个缺陷"**：

- **18 的第一次实现就是错的**。最初写的是"已发送 → 禁用按钮，等 hub 回答"。但 hub 如果永远不回答（决策丢失），按钮就永远禁用——和 D18 一模一样，只是安静一点。改成**从不禁用**之后，这个类别整体消失：一个只能重发的按钮不会卡住。
- **`stats()` 这个 store 方法是个陷阱，而且咬了两次。** P4 就发现"返回新对象的方法不能当 selector 用"并写了注释，P6 写 Inspector 时**又**用它当 selector，React 直接 185 崩掉。第二次之后处理方式变了：不复述规则，而是**把方法删掉**（改成 `statsFor(state, id)` 自由函数），组件只能走 `useView` + `statsOf`。规则可以忘，不存在的 API 忘不掉。
- **桩服务不能靠 `page.route` 断网。** WebSocket upgrade 不是 Playwright 路由拦截的 HTTP 请求，所以"D28 的 HTTP 回退"这条测试一开始是**假通过**的——转录本来就在屏幕上。给桩服务加了 `/__stub/down`（拒绝 upgrade 并关闭现有连接），并让断言检查 `/events` 请求真的发出过。测试通过得容易时，值得问一句它到底证明了什么。

### 6.4 P7 期间新发现的两个缺陷（都是"看起来正常"的那种）

两条都是**类型检查通过、构建通过、浏览器行为测试通过**，只有截图才看得出来的缺陷。记在这里是因为它们的形状相同：
失败的样子不是报错，而是"少了一点东西"。

**N8 · JSX 属性写成字符串拼接，等于把后半行当成类名文本。**
`overlays.tsx` 的对话框写的是：

```jsx
className="fixed left-1/2 top-1/2 z-50 w-[min(32rem,calc(100vw-2rem))] `
    + `-translate-x-1/2 -translate-y-1/2 rounded-lg border border-line `
    + `bg-surface p-4 shadow-xl focus:outline-none"
```

JSX 里 `attr="a" + \`b\`` **不是拼接**：属性值在第二个引号处结束，后面的内容（连反引号、换行、加号一起）被当成字符串的一部分。
所以这个对话框从 P4 起就只有一个真实类名列表，`-translate-x-1/2`（水平居中）、`bg-*`（背景）、`p-4`、`shadow-xl` 全部没生效——
它是一个没有背景、向右偏移的对话框。P5 已经在 `Markdown.tsx` 撞见过同一个语法并修好，但没有回头搜一遍。
现在 `panel-theme.test.js` 直接扫这个形状（`="…\`" 后跟行首 `+`），并且浏览器测试量了对话框的实际几何与背景色。

**N9 · `@theme` 块内 `--color-*: initial` 写在自定义 token 之后，会把它们一起清掉。**
`--color-*: initial` 是"关掉 Tailwind 自带调色板"，但同一个 `@theme` 块内**按顺序处理**，写在面板 token 后面就把面板 token 也清了。
后果是构建产物里**一个颜色工具类都没有**（`.bg-surface`、`.text-ink`、`.border-line` 全部缺失）。
而它看起来是正常的：`body` 上有 `background: var(--app)` 与 `color: var(--ink)`，深色主题照样"生效"，
所有面板、卡片、对话框只是**静默透明**。修法是把 `--color-*: initial` 拆进前一个独立的 `@theme` 块；
`panel-theme.test.js` 断言两者不在同一个块里。

两个缺陷都做了还原验证：把代码改回缺陷形态，对应测试确实失败。

**对比度是量出来的**。`panel-contrast.spec.ts` 在渲染后的页面上取计算色（用浏览器自己的颜色解析器把 `oklch(...)` 过一遍 canvas），
按 WCAG 公式算比值，明暗两套主题各跑一遍，顺带审计结构性可访问性（每个控件有无障碍名、landmark、标题层级不跳级、图标要么 `aria-hidden` 要么有名字）。
第一次跑就抓到 `--ink-faint` 在浅色下只有 3.65:1（12px 文本要求 4.5:1），于是改的是 token 而不是阈值。

### 6.5 P8：收尾时的四个决定

**D1 · 不加 SPA fallback。** 计划 §3.2 写的是"改指向 `web/dist` 并加 fallback"，做的时候发现前提不成立：
面板是单页且**没有客户端路由**，`?session=` 与 `?token=` 都是查询参数，所以"路径不认识"本来就该是 404。
加一个"任何路径都回 `index.html`"的 fallback 只会把打错的资源路径变成一份 200 的 HTML——而那正是
"脚本没加载"最难查的形状。真的加了路由再补，也不迟。

**D2 · 未构建时回 503，而不是 404。** 从 git clone 下来直接 `npm start` 是真实会发生的事，此时
`web/dist` 不存在。原来的行为是一个毫无线索的 404；现在是一个 503，正文里写着 `npm install && npm run build`。
这不是错误处理，是可诊断性：hub 启动得好好的、却对自己的首页回 404，是运维分不清"部署坏了"还是"URL 写错了"的形状。

**D3 · 缺 `Origin` 的 upgrade 继续放行，并且把理由写进代码。** 计划写的是"补齐 Origin 缺失的分支"，
语气默认那个分支应该拒绝。实际上**缺 Origin ≠ Origin 不对**：浏览器一定会发，所以缺失只可能是非浏览器客户端
（`wscat`、脚本、别的运维工具），而那种客户端要么已经持有面板 token、要么本来就能直接调 HTTP API——
拒绝它只是把这份协议里"或任何运维工具"那一半砍掉，换不来任何安全。真正值得点名的是 sandboxed frame，
它发的是字面量 `null`，而 `new URL('null')` 抛错后 `originHost` 为 null，与 host 不等，**已经被拒绝**。
结论写进了 `src/panel/api.ts` 的注释里，因为下一个人读到的应该是论证而不是一条沉默的分支。

**D4 · CSP 的 hash 从文档里算，不写死在头里。** 面板有一个内联脚本（主题，必须在 bundle 之前跑，
否则深色偏好每次刷新都闪白）。写死 hash 的 CSP 会在那个脚本改一个字之后静默失效——而"脚本被 CSP 挡了"
在浏览器里表现为主题不生效，不是白屏，很容易被当成别的问题。所以 `panelHeaders()` 每次响应读一遍
`web/dist/index.html`，算出内联脚本的 sha256 放进 `script-src`。代价是每个文档请求多一次读文件（本地磁盘、
几 KB、`no-cache` 本来就不缓存），换来的是策略与文档**不可能**不一致。
`style-src` 保留 `'unsafe-inline'`，理由写在同一处：Radix 用 React 写 `style` 属性定位浮层，
去掉它每个菜单和对话框都会跑到左上角。

顺带把 hash 资产的缓存补上了（计划 §3.2 里的一条）：`index-<hash>.js` 一年 immutable，文档本身 no-cache。
这在这里不是优化问题，是 230 KB gzip 的面板每次刷新都重下的问题。

**证据**：`npm test` 297、`npx playwright test` 47、`npm run test:e2e` 2、三份 tsconfig 全清；
`npm run check:panel` 现在**直接指向 hub 自己服务出来的面板**（不再有 preview 代理夹在中间），
建会话 → 启动 → 发消息 → 审批 → 工具卡 → 刷新重放全流程通过，`{events:14, gaps:0, duplicates:0, protocolErrors:0}`，
console error 为零——CSP 是否挡住了什么，这一条就是证据。

### 6.3 P5 期间新发现的三个问题

**N5 · `tool_results` 的条目不是文档写的 Result object，而 P4 的移植把这件事忘了。**
`core/docs/worker-protocol.md` 的"Result object"一节写的是 `{query, output, extras}`，但实测（真实 worker 的原始载荷）是 `{content, invoke_return: {query, output}, role, type}`——一个 `invoke_return` **工具消息**，provenance 嵌在 `invoke_return` 里。

旧面板**知道**这件事，而且在代码里写清楚了（`web/js/render.js:353-362`）：

> The documented shape is a Result object (`{query, output, extras}`). Core currently projects results as tool messages instead (`{content, invoke_return, role, type}`), so both are accepted.

P4 把 `state.js`/`api.js` 移植成了 TypeScript，却**没有移植 `render.js`**——而这条知识只存在于 `render.js`。于是 P4 的工具结果读出来是 `(unnamed call)`、没有参数、没有输出（P4 的截图里就是这样的），当时没注意到，因为 P4 的验收只看了"转录还在不在"。

教训是具体的：**移植时丢掉一条注释，等于丢掉一个已经付过学费的事实**。教训也是可操作的：`content.ts` 现在两种形状都读，并把那句话抄在了函数头上；`test/panel-transcript.test.js` 的夹具用的是**真实载荷**的形状（从一次实跑的 `GET /api/sessions/:id/events` 里抄的），不是文档的形状。

**N6 · 高亮的代价是固定的，而且限制不了。**
`rehype-highlight` 静态 `import { common } from 'lowlight'`，所以 highlight.js 的 common 语言集**总是**进包：实测 608 KB / 187 KB gzip，去掉整个 rehype-highlight 是 441 KB / 134 KB gzip——即 markdown + 高亮一共约 53 KB gzip，其中高亮约 53 KB 里的大部分。传 `languages: {}` **不会**变小（导入是静态的），所以"只注册常用语言"这个念头在打包层面是无效的，只有自己建 lowlight 实例才做得到。

结论是接受：这是跑在 `127.0.0.1` 的本机工具，§2.1 已经写明体积不是约束，而 187 KB gzip 对任何标准都不算大。数字记在这里，是因为下一个想加依赖的人应该看到它。

**N7 · 工具结果的"字段"和"段落"是显示约定，不是机器协议。**
生产者自己的头文件（`utils/textformat/include/textformat/document.hpp`）写着：

> The markers distinguish metadata visually without Markdown headings or fences; **they are not a machine protocol or a security boundary.**

所以 `toolOutput.ts` 的定位是**显示启发式**：识别得了就分开画，识别不了就原样显示，永远保留 `raw output`，并且**不从解析结果里推导成功、失败或安全性**——成功/失败只来自 `extras.error` 与 `extras.loop_skipped`，那是协议里真正定义的。这条边界是刻意画的：一个把 `[[exit_code]]: 0` 读成"成功"的面板，会在工具换了输出格式之后安静地开始说谎。

---

## 7. 风险与取舍

| 风险 | 影响 | 应对 |
| --- | --- | --- |
| **引入构建步骤** | 打破"改完刷新即可"与"无构建部署" | `vite dev` 的 HMR 反而更快；生产走多阶段构建 |
| **依赖从 1 个变成几十个** | 供应链面、`npm ci` 变慢 | 运行时依赖仍只有 `ws`；前端依赖全是构建期产物；锁文件 + CI 审计 |
| **Node floor 提升到 22.18** | 放弃 Node 20（2026-04-30 已 EOL）；本机与 CI 都需要 ≥22.18 | 已接受。这是 P1 实测后唯一自洽的选择：见 §9。缓解是 CI 矩阵仍测 floor 本身（`22.18`）而非只测最新版 |
| **`test/e2e/panel.test.js` 会失效** | 它断言 `#timeline article.card`、`section.run-group`、`#panel-badge`、`#theme-toggle`、Approve 按钮文本 | ✅ **已随 P8 删除**（连同只服务于它的 CDP helper `test/helpers/browser.js`）。它没有按计划在 P4 失效，因为旧面板在 P4–P7 期间一个字节都没改——比计划更好：迁移期间旧面板继续可用且有测试守着 |
| **`panel-assets.test.js` 的安全契约** | markdown 渲染看似与"禁止写 HTML"冲突 | `react-markdown` 不经 `innerHTML`；契约按 §2.4 重写并保留语义 |
| **重写期间双份前端** | 维护成本 | 时间盒；旧面板冻结，只修安全级 bug |
| **P0 加固与重构并行** | 两处改动互相冲突 | P0 先合入并单独发布；P3 的 TS 化以加固后的代码为基线 |
| **Tailwind 类名可读性** | C++ 背景的维护者可能反感 | 备选 CSS Modules（§8 决策点 2） |

**明确不做**：不引入 SSR/Next、不做多用户/登录/RBAC、不改 worker 协议、不动 C++、不重写后端架构（只做 TS 化 + P0 修复，模块划分照搬）。

---

## 8. 待确认的决策点

请逐条给出选择；默认项已标注。

1. **前端框架**：React 19（默认）／ Vue 3.5 ／ Svelte 5。
2. **样式方案**：Tailwind v4 + Radix（默认）／ CSS Modules + 自研无头组件。
3. ~~**后端 TS 运行方式**~~ ✅ **已定：Node 原生跑 TS，`engines` 提到 ≥22.18**。原选项"保留 Node 20.11"经 P1 实测证明与共享 TS 契约不可兼得，理由与证据见 §9。
4. **Markdown 高亮**：highlight.js（默认，轻）／ Shiki（更好看，体积更大）。
5. **P0 加固是否先单独合入**：是（默认，建议）／ 与重构一起做。
6. **协议能力首版范围**：只做 `event-page` + `transcript-delta` 预留（默认）／ 连 `audit-log`、`session-search` 一起实现。
7. **旧面板处理**：P4–P7 并行保留、P8 删除（默认）／ 直接切换，不留双份。
8. **是否接受"无真流式"**：接受面板侧渐进呈现、C++ 不改（默认）／ 需要真流式则必须改 worker 协议（超出本次范围）。

---

## 9. Node floor：从 20.11 提到 22.18（已决定）

决策点 3 原本选的是"`tsc` 产物 + 保留 Node 20.11 兼容"。**P1 落地后实测发现这两件事无法同时成立**，于是 floor 提到了 22.18。这一节保留证据，因为它是后端的形状（零构建、无 `dist/`）的依据。

| 事实 | 验证方式 |
| --- | --- |
| Node 20.11 **无法加载 `.ts` 文件** | `ERR_UNKNOWN_FILE_EXTENSION: Unknown file extension ".ts" for hub/shared/protocol.ts` |
| Node 20.11 **仍能运行 hub 本身** | `node bin/simplex-hub.js --version` → `0.1.0`（因为 `src/` 还没有导入 `shared/`） |
| Node 24 **能直接跑 `.ts`**，且 `.js` 导入 `.ts` 可行 | `test/protocol-constants.test.js` 在 24 上 200 项全绿 |
| Vite 8 要求 Node **≥20.19** | 20.11 上 `vite build` 直接崩：`node:util` 没有 `styleText` 导出 |
| Vitest 5 要求 Node **≥22.12** | 包元数据 |

关键点是第一条与第三、四条的**不对称**：`shared/protocol.ts` 已经把有效 floor 推到 22.18，但因为当时 `src/` 还没导入它，20.11 的运行时仍然能用——CI 里那条 job 之所以是绿的，靠的是一个具名 skip：

```
ok 1 - panel protocol constants # SKIP type stripping needs Node >= 22.18; this is 20.11.1
```

P2 会让 `src/` 导入共享契约，那一刻 skip 就失效了。而开发工具（Vite/Vitest）**本来就**要求 ≥22.12——保持 20.11 只能保住运行时、保不住工具链。

**曾评估的替代方案**：保持 20.11，让 `tsc` 把 `shared/` 编译成 `shared/*.js` + `.d.ts`，`src/` 导入 `.js`。否决理由：产物要么提交（必然漂移）要么每次构建（"零构建"这个 hub 现存的优势消失），而它换来的只是一个 2026-04-30 已 EOL 的版本；且工具链仍需 ≥22.12，CI 照样要拆两个 job。

**落地结果**：`engines` = `>=22.18`；CI 矩阵 `['22.18', '24']`；`test/protocol-constants.test.js` 的 skip 守卫**已删除**——floor 现在保证 type stripping，所以该测试无条件运行，将来若有人误动 floor 或矩阵，它会直接失败而不是静默跳过。

---

## 附录：证据位置

- 后端崩溃缺陷：`hub/src/http/server.js:74,78`、`hub/src/panel/api.js:617`、`hub/src/launch/supervisor.js:193,210,233,270,345-355`
- 前端灾难级缺陷：`hub/web/js/app.js:446-457`（A1 订阅）、`hub/src/panel/api.js:130-135,464-470`、`hub/web/js/state.js:150-152,352`（A2 重放）、`hub/web/css/app.css:476` vs `hub/web/js/app.js:1025`（A3 hidden）
- 协议定义：`hub/docs/hub-protocol.md`、`hub/src/hub.js:23-32`、`hub/src/panel/api.js`
- Worker 协议（不改动）：`core/docs/worker-protocol.md`、`hub/src/protocol/events.js`
- 受影响的测试：`hub/test/panel-assets.test.js`、`hub/test/e2e/panel.test.js`、`hub/test/protocol-drift.test.js`
- 构建与部署：`hub/package.json`、`hub/src/http/static.js`、`docker/Dockerfile.hub-test`、`.github/workflows/ci.yml`
