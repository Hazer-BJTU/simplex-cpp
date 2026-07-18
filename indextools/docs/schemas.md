# 公共返回结构

命令的 `result` 复用一组公共结构。本文件是这些结构的权威定义；各命令在 commands.md 中引用此处。

## 1. DisplayBlock[]

源内容类响应（检索、查看、diff）统一返回 `DisplayBlock[]`。与 C++ 侧 `include/schema.hpp` 完全一致。

每个 block：

```json
{
  "meta": {
    "field_name":    ["File", "Lines", "Total Lines", "Truncated"],
    "field_content": ["/abs/path", "[40, 79]", 1200, true]
  },

  "text": {
    "line_content": ["line a", "line b"],
    "line_number":  [40, 41],
    "line_type":    ["base", "match"]
  },

  "sub_entity": [ /* DisplayBlock[]，仅实体树 */ ]
}
```

规则：

- `meta` 必有；`field_name[i]` 标注 `field_content[i]`，值可为 string/number/bool。范围编码为字符串 `"[start, end]"`（end 闭区间）。
- **body 二选一**：
  - `text`（行级）：三个平行数组，`line_number` 为 0-based 行号。用于 `search.*`、`file.read_lines`、`edit.*` 的 diff。
  - `content`（整串）：单个原始字符串，不切行。用于 `file.read_bytes`。
- `sub_entity` 可选，仅实体树（`search.locate_entity`）出现，递归为 `DisplayBlock[]`。

### line_type 取值

| 值 | 含义 |
|---|---|
| `base` | 普通/上下文行。 |
| `match` | 命中搜索的行。 |
| `add` | diff 新增行。 |
| `delete` | diff 删除行。 |

## 2. ProcessReport[]

子进程状态类响应返回 `ProcessReport[]`。与 `include/schema.hpp` 一致。

```json
{
  "meta": {
    "field_name":    ["ID", "Description", "Status", "Exit Code", "Elapsed (ms)"],
    "field_content": [0, "build", "running", null, 128]
  },

  "stdout": "hello\n",
  "stderr": null
}
```

规则：

- `meta` 必有。`Status` ∈ `running` | `finished` | `exited` | `unknown`；`Exit Code` 退出后为整数，否则 `null`。
- `stdout` / `stderr` 成对出现或成对缺失：
  - `list`/`status` 类命令只返回 `meta`。
  - `collect_*` 类命令附带 `stdout`/`stderr`（各为字符串或 `null`）。

## 3. 二进制块

二进制/图片传输返回下列两种形态之一（见 commands.md `binary.*`）。

### 3.1 内联（小资源）

```json
{
  "type": "binary",
  "mime": "image/png",
  "encoding": "base64",
  "size": 20480,
  "data": "iVBORw0KGgo..."
}
```

### 3.2 引用（大资源，分块拉取）

```json
{
  "type": "binary_ref",
  "resource_id": "bin-42",
  "mime": "image/png",
  "size": 10485760,
  "chunk_size": 65536
}
```

分块响应（`binary.read_chunk` 的 `result`）：

```json
{
  "resource_id": "bin-42",
  "offset": 0,
  "length": 65536,
  "eof": false,
  "encoding": "base64",
  "data": "..."
}
```

| 字段 | 说明 |
|---|---|
| `mime` | 猜测的 MIME 类型（按扩展名/魔数）。 |
| `encoding` | 当前恒为 `base64`。 |
| `size` | 资源总字节数。 |
| `chunk_size` | 引用形态下建议的分块大小。 |
| `eof` | 分块响应中，`true` 表示本块是最后一块。 |

内联 vs 引用的阈值由服务端 `--inline-binary-limit` 控制（见 server.md）。
