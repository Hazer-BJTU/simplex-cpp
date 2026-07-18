# 协议：传输、Envelope 与错误模型

本文件定义服务端与客户端之间的线协议，独立于具体传输（stdio / TCP / WebSocket）。

## 1. 传输帧（第一阶段：stdio NDJSON）

- 请求与响应均为 **单行 JSON**，以 `\n` 结束（NDJSON）。
- JSON 字符串内部的换行会被转义为 `\n`，因此单行帧对任意文本/diff 内容都安全。
- 服务端逐行读取 stdin，每读到一条完整 JSON 请求就处理并向 stdout 写回一条 JSON 响应。
- 空行、纯空白行被忽略。
- 无法解析为 JSON 的行返回一条 `parse_error` 响应（`id` 为 `null`），不断开连接。
- stdout 只输出协议响应；所有日志、诊断信息写 stderr。

一问一答默认按行顺序对应，但客户端应始终以响应中的 `id` 做匹配，不要依赖顺序（未来传输层可能乱序/并发返回）。

## 2. 请求 Envelope

```json
{
  "id": "req-001",
  "token": "client-a",
  "method": "file.read_lines",
  "params": { "path": "src/main.cpp", "line_start": 0, "max_lines": 40 }
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `id` | string \| number \| null | 否 | 请求标识，原样回显到响应。缺省视为 `null`。建议客户端始终提供以便匹配。 |
| `token` | string | 否 | 会话令牌。缺省使用服务端配置的默认会话（见 server.md `--default-token`）。同一 `token` 复用同一会话状态。 |
| `method` | string | 是 | 命令名，形如 `namespace.action`（见 commands.md）。 |
| `params` | object | 否 | 命令参数对象。缺省视为 `{}`。 |

约束：

- 顶层必须是 JSON object。
- `method` 必须是已注册命令，否则返回 `unknown_method`。
- `params` 若存在必须是 object，否则返回 `invalid_request`。

## 3. 响应 Envelope

成功：

```json
{
  "id": "req-001",
  "ok": true,
  "result": [ /* 命令相关，见各命令 */ ]
}
```

失败：

```json
{
  "id": "req-001",
  "ok": false,
  "error": {
    "code": "invalid_params",
    "message": "params.line_start must be a non-negative integer",
    "details": { "field": "line_start" }
  }
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | string \| number \| null | 回显请求 `id`；无法解析请求时为 `null`。 |
| `ok` | boolean | 成功为 `true`，失败为 `false`。 |
| `result` | any | 仅 `ok=true` 时存在。类型由命令定义（多为 `DisplayBlock[]` / `ProcessReport[]` / 对象）。 |
| `error` | object | 仅 `ok=false` 时存在。见下。 |

`error` 对象：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `code` | string | 是 | 机器可读错误码（见第 4 节）。 |
| `message` | string | 是 | 人类可读描述。 |
| `details` | object | 否 | 附加上下文（如出错字段名、路径）。 |

## 4. 错误码

| code | 触发场景 |
|---|---|
| `parse_error` | 请求行不是合法 JSON。 |
| `invalid_request` | 顶层不是 object、`method` 缺失或非字符串、`params` 非 object。 |
| `unknown_method` | `method` 未注册。 |
| `invalid_params` | 参数缺失、类型错误或取值非法。`details.field` 指出具体字段。 |
| `path_denied` | 路径逃逸出会话 `root_path`，或访问被拒。 |
| `not_found` | 目标文件/进程/二进制资源不存在。 |
| `edit_conflict` | 编辑无法唯一定位（`str_replace` 0 个或多个匹配等）。 |
| `io_error` | 读写文件失败。 |
| `internal_error` | 未归类的服务端内部错误。 |

所有错误均以 HTTP 无关的 envelope 返回；服务端不因单条命令失败而中断会话。

## 5. `token` 与会话语义

- 首次出现的 `token` 惰性创建一个会话，绑定一个 `root_path`（见 server.md 与 `session.open`）。
- 同一 `token` 的后续请求复用该会话的：检索缓存、子进程集合、二进制资源表、会话配置（默认上下文行数等）。
- 不同 `token` 之间完全隔离：A 的子进程/二进制资源对 B 不可见。
- 会话可通过 `session.close` 主动关闭，或由服务端在空闲超时后回收（见 server.md `--session-idle-timeout`）。
- `token` 的 `root_path` 在会话生命周期内固定；要换根目录需新建/切换 `token` 或先 `session.close`。

## 6. 路径安全约定

所有接受 `path` 的命令遵循统一规则：

1. 相对路径相对会话 `root_path` 解析；允许绝对路径但同样要落在 `root_path` 内。
2. 服务端对最终路径做 `weakly_canonical`，并校验其仍位于 `root_path` 之下。
3. 拒绝 `..` 逃逸、符号链接逃逸、越界绝对路径 → 返回 `path_denied`。
4. 检索命令的 glob 展开同样被限制在 `root_path` 内。

## 7. 版本与兼容

- 本协议版本可通过 `server.info` 命令查询（返回 `protocol_version`）。
- 新增字段向后兼容；客户端应忽略未知字段。
- 破坏性变更会提升 `protocol_version` 并更新本目录。
