# 命令参考

本文件逐一说明第一阶段支持的命令。每条命令给出：功能、`params` 字段、`result` 结构、错误。公共返回结构见 [schemas.md](schemas.md)，envelope 与错误码见 [protocol.md](protocol.md)。

命令命名空间总览：

| 命名空间 | 用途 |
|---|---|
| `server` | 服务端能力探测与关停。 |
| `session` | 会话生命周期与配置。 |
| `search` | 代码检索（基于语言插件 + 缓存）。 |
| `file` | 文件查看（行/字节）。 |
| `edit` | 文件编辑（默认 dry-run，返回 diff）。 |
| `process` | 子进程管理。 |
| `binary` | 二进制/图片资源传输。 |

---

## server 命名空间

### `server.info`

探测服务端能力。无副作用，不需要会话。

**params**：无（忽略）。

**result**：

```json
{
  "name": "indextools",
  "protocol_version": 1,
  "server_version": "0.1.0",
  "supported_methods": ["server.info", "search.locate_pattern", "..."],
  "supported_extensions": [".py", ".pyi", ".cpp", "..."],
  "plugins": [{ "name": "Python", "extensions": [".py", ".pyi", ".pyw"] }]
}
```

| 字段 | 说明 |
|---|---|
| `protocol_version` | 线协议版本整数。 |
| `supported_methods` | 已注册命令名数组。 |
| `supported_extensions` | 当前加载的语言插件覆盖的扩展名。 |
| `plugins` | 已加载插件列表（来自 `LangPluginManager`）。 |

### `server.shutdown`

请求服务端优雅关停：终止所有会话的子进程、关闭会话、刷新 stdout 后退出。

**params**：无。

**result**：`{ "shutting_down": true }`（在退出前写出）。

仅当服务端以 `--allow-shutdown` 启动时可用（见 server.md），否则返回 `unknown_method`。

---

## session 命名空间

### `session.open`

显式创建/绑定会话并设定其 `root_path`。惰性创建也可发生在任意带新 `token` 的请求上，但那时会用服务端默认 root；`session.open` 用于精确指定。

**params**：

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `root_path` | string | 否 | 服务端 `--root` | 该会话所有文件/检索/编辑操作的根目录。必须存在且为目录。 |
| `context_lines` | integer≥0 | 否 | 服务端 `--context-lines` | 检索结果默认上下文行数。 |
| `num_tasks` | integer≥1 | 否 | 服务端 `--search-tasks` | 该会话检索并发 fan-out 宽度。 |

若 `token` 已存在且 `root_path` 与现有会话不同 → 返回 `invalid_params`（需先 `session.close`）。

**result**：

```json
{
  "token": "client-a",
  "root_path": "/abs/root",
  "context_lines": 3,
  "num_tasks": 4,
  "created": true
}
```

`created` 表示本次是否新建（`false` 表示复用已有会话且参数一致）。

### `session.info`

返回当前会话状态。

**params**：无。

**result**：

```json
{
  "token": "client-a",
  "root_path": "/abs/root",
  "context_lines": 3,
  "num_tasks": 4,
  "running_processes": 2,
  "binary_resources": 1,
  "cached_files": 37
}
```

### `session.set_config`

调整会话级默认值。仅影响后续命令。

**params**：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `context_lines` | integer≥0 | 否 | 更新默认上下文行数。 |

**result**：更新后的配置对象（同 `session.info` 的配置字段子集）。

### `session.close`

关闭当前会话：终止其所有子进程、释放二进制资源、清空缓存、解绑 `token`。

**params**：无。

**result**：

```json
{ "token": "client-a", "closed": true, "terminated_processes": 2 }
```

对未知 `token` 返回 `{ "closed": false }`（非错误）。

---

## search 命名空间

三条检索命令都作用于 `root_path` 下由 `glob` 选中的文件集合，经语言插件分析 + 缓存后合并结果。底层为 `SearchInterface`（`include/cache_system.hpp`）。全部返回 `DisplayBlock[]`（见 schemas.md）。

所有 search 命令共享参数：

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `glob` | string | 否 | `**/*` | 相对 `root_path` 的 glob，选中待检索文件。 |
| `root_path` | string | 否 | 会话 root | 覆盖本次检索的根（仍受路径安全约束）。 |

无插件认领的文件被跳过；单文件分析异常被吞掉，不影响整批。结果为跨文件合并的扁平数组，顺序不保证稳定。

### `search.locate_pattern`

按子串或正则在选中文件的行内检索。

**params**（除共享参数外）：

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `pattern` | string | 是 | — | 子串；当 `use_regex=true` 时为 ECMAScript 正则。 |
| `use_regex` | boolean | 否 | `false` | 是否按正则解释 `pattern`。 |

**result**：`DisplayBlock[]`，每块 `text` body，命中行 `line_type="match"`，其余为上下文 `base`（数量由 `context_lines` 控制）。

### `search.locate_identifier`

检索一个已在分析期建立索引的标识符出现位置（比裸文本更精确，跳过注释/字符串等取决于插件实现）。

**params**（除共享参数外）：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `identifier` | string | 是 | 目标标识符名。 |

**result**：`DisplayBlock[]`（`text` body，命中行标 `match`）。

### `search.locate_entity`

按实体键定位一个命名实体（函数/类/模块等）及其完整子树。

**params**（除共享参数外）：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `entity_key` | string | 是 | 实体查找键（如函数名、类名、限定名）。 |

**result**：`DisplayBlock[]`，每个匹配实体一块，可含递归 `sub_entity`。

---

## file 命名空间

只读查看，底层为 `viewer.hpp`。全部返回 `DisplayBlock[]`（恰好一块）。均分页感知：`meta` 报告总量与 `Truncated`。

### `file.read_lines`

按 0-based 行号读取半开窗口 `[line_start, line_start + max_lines)`。行级查看，AI 编辑/diff 的主要入口。

**params**：

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `path` | string | 是 | — | 目标文件（受路径安全约束）。 |
| `line_start` | integer≥0 | 否 | `0` | 起始行 0-based。 |
| `max_lines` | integer≥0 | 否 | `200` | 最大返回行数（页大小）。`0` 返回空 body 但仍报告总量。 |

**result**：一块 `DisplayBlock`，`text` body，全部行 `line_type="base"`。`meta` 含 `File` / `Lines`（返回区间）/ `Total Lines` / `Truncated`。

### `file.read_bytes`

按字节偏移读取半开窗口 `[byte_start, byte_start + max_bytes)`，作为单个原始字符串返回（不切行）。用于压缩单行文件、精确切片、二进制/大文件续读。

**params**：

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `path` | string | 是 | — | 目标文件。 |
| `byte_start` | integer≥0 | 否 | `0` | 起始字节偏移。 |
| `max_bytes` | integer≥0 | 否 | `65536` | 最大返回字节数。`0` 返回空 content 但仍报告总量。 |

**result**：一块 `DisplayBlock`，`content` body。`meta` 含 `File` / `Bytes` / `Total Bytes` / `Truncated` / `Binary`（切片含 NUL 时为 `true`）。

> 提示：真正的二进制/图片下发用 `binary.*`，`file.read_bytes` 更适合文本切片；对二进制文件它会以字符串形式返回原字节，可能有损。

---

## edit 命名空间

编辑底层为 `editor.hpp` 的纯函数（`line_replace_edit` / `str_replace_edit` / `check_difference`）。

**默认 dry-run**：编辑命令默认 **不写盘**，只计算并返回 diff。要真正落盘需显式 `apply: true`（且服务端以 `--allow-write` 启动，见 server.md）。落盘前会重新读取文件并校验其与请求时一致，避免覆盖并发修改。

编辑成功统一返回：

```json
{
  "path": "/abs/path",
  "applied": false,
  "diff": [ /* DisplayBlock[]，line_type 为 add/delete/base */ ]
}
```

| 字段 | 说明 |
|---|---|
| `applied` | 是否真的写盘。dry-run 恒为 `false`。 |
| `diff` | `check_difference()` 生成的 `DisplayBlock[]`：删除块 + 新增块，行标 `delete`/`add`，上下文 `base`。 |

### `edit.line_replace`

替换或插入连续行区间；行尾归一为 LF。

**params**：

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `path` | string | 是 | — | 目标文件。 |
| `line_start` | integer≥0 | 是 | — | 起始行 0-based。 |
| `line_end` | integer≥0 | 否 | `line_start` | 结束行 0-based，**闭区间**。替换模式必填语义；插入模式忽略。 |
| `content` | string | 是 | — | 新内容（会按默认分隔符重切并以 LF 重连）。 |
| `insert_mode` | boolean | 否 | `false` | `true`：在 `line_start` 之后插入，忽略 `line_end`；`line_start` 超尾则追加。`false`：替换闭区间 `[line_start, line_end]`。 |
| `apply` | boolean | 否 | `false` | `true` 且服务端允许写时落盘。 |

**错误**：范围越界 → `invalid_params`；空源上的替换模式 → `invalid_params`；写盘失败 → `io_error`。

### `edit.str_replace`

对源做精确子串替换。

**params**：

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `path` | string | 是 | — | 目标文件。 |
| `old` | string | 是 | — | 被替换的原始子串。 |
| `new` | string | 是 | — | 替换后的内容。 |
| `replace_all` | boolean | 否 | `false` | `false` 要求唯一匹配，0 个或多个匹配 → `edit_conflict`；`true` 替换全部。 |
| `apply` | boolean | 否 | `false` | 是否落盘。 |

**错误**：非唯一匹配（`replace_all=false`）→ `edit_conflict`（`details` 含匹配数）。

### `edit.diff`

对给定文件与一段候选内容做行级 diff，不改动任何东西（纯预览，不需要 `apply`）。

**params**：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `path` | string | 是 | 原始文件。 |
| `content` | string | 是 | 候选新内容（整文件）。 |
| `context_lines` | integer≥0 | 否 | 覆盖会话默认上下文行数。 |

**result**：`{ "path": ..., "diff": DisplayBlock[] }`（无 `applied`）。

---

## process 命名空间

会话级子进程管理，底层为 `SubProcessManager`（`include/subprocess.hpp`）。子进程按 `token` 隔离：只能看见/操作本会话自己 spawn 的进程。`ProcessID` 为会话内 uint64。

仅当服务端以 `--allow-process` 启动时可用（见 server.md）。

### `process.spawn`

启动子进程。`exec` 经 PATH 解析。

**params**：

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `exec` | string | 是 | — | 可执行文件名/路径。 |
| `args` | string[] | 否 | `[]` | 参数列表。 |
| `description` | string | 否 | `exec` | 状态报告中的人类可读标签。 |

**result**：`{ "id": 3, "description": "build" }`。

> 若服务端以 `--exec-allowlist` 启动，`exec` 不在白名单 → `path_denied`。

### `process.status`

列出本会话所有存活进程的状态（仅 meta，不含输出）。

**params**：无。

**result**：`ProcessReport[]`（仅 `meta`，见 schemas.md）。

### `process.collect_running`

快照当前运行中进程的状态与输出，**不移除**它们。

**params**：

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `full_output` | boolean | 否 | `false` | `true` 返回完整 stdout/stderr；`false` 只返回自上次 collect 以来的增量。 |

**result**：`ProcessReport[]`（含 `stdout`/`stderr`）。

### `process.collect_finished`

收集并 **移除** 已结束的进程，释放其 ID。

**params**：

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `full_output` | boolean | 否 | `false` | 同上，完整 vs 增量输出。 |

**result**：`ProcessReport[]`（含 `stdout`/`stderr`）。已结束进程被移除。

### `process.write_stdin`

向指定进程 stdin 写入数据。

**params**：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `id` | integer | 是 | 目标进程 ID。 |
| `data` | string | 是 | 写入内容。 |

**result**：`{ "id": 3, "written": true }`。stdin 已关闭时 `written=false`；未知 id → `not_found`。

### `process.close_stdin`

关闭指定进程的 stdin（发送 EOF）。

**params**：`{ "id": integer }`。

**result**：`{ "id": 3, "closed": true }`。

### `process.terminate`

对指定进程发送 SIGTERM 并回收。

**params**：`{ "id": integer }`。

**result**：`{ "id": 3, "terminated": true }`；未知 id → `not_found`。

### `process.terminate_all`

终止并回收本会话所有运行中进程（用于清理）。

**params**：无。

**result**：`{ "terminated": 2 }`。

---

## binary 命名空间

二进制/图片传输。小资源内联返回，大资源返回句柄后分块拉取，避免单条 JSON 响应过大。二进制块结构见 [schemas.md](schemas.md) 第 3 节。资源按会话隔离。

### `binary.read`

读取一个二进制文件。服务端按 `--inline-binary-limit` 决定内联还是转为引用。

**params**：

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `path` | string | 是 | — | 目标文件（受路径安全约束）。 |
| `force_ref` | boolean | 否 | `false` | `true` 强制返回引用形态，即使小于内联阈值。 |

**result**：`binary`（内联）或 `binary_ref`（引用）块。引用形态会在会话内登记一个 `resource_id`，直至 `binary.release` 或会话关闭。

### `binary.read_chunk`

按偏移拉取一个引用资源的分块。

**params**：

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `resource_id` | string | 是 | — | `binary.read` 返回的资源句柄。 |
| `offset` | integer≥0 | 否 | `0` | 起始字节偏移。 |
| `max_bytes` | integer≥1 | 否 | 资源 `chunk_size` | 本次最大字节数。 |

**result**：分块对象 `{ resource_id, offset, length, eof, encoding, data }`（见 schemas.md 3.2）。未知句柄 → `not_found`。

### `binary.release`

释放一个引用资源。

**params**：`{ "resource_id": string }`。

**result**：`{ "resource_id": "bin-42", "released": true }`；未知句柄返回 `released: false`（非错误）。







