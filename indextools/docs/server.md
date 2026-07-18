# 服务端启动参数

服务端可执行文件为 `indextools`（构建产物在 `bin/indextools`），用 Boost.program_options 解析参数（CMake 已链接 `Boost::program_options`）。第一阶段只实现 stdio 交互模式。

## 运行模式

| 参数 | 说明 |
|---|---|
| `--stdio` | 交互模式：从 stdin 逐行读 NDJSON 请求，向 stdout 逐行写响应。第一阶段的主模式。 |
| `--tcp <host:port>` | （预留，后续阶段）TCP NDJSON 服务。**当前实现**：解析该参数但启动即以退出码 `2` 报错，尚未实现。 |

未显式指定模式时，默认 `--stdio`。

## 会话与根目录

| 参数 | 默认 | 说明 |
|---|---|---|
| `--root <path>` | 当前工作目录 | 会话未显式 `session.open` 时使用的默认 `root_path`。所有路径安全校验以会话 root 为界。 |
| `--default-token <str>` | `default` | 请求缺省 `token` 时使用的会话令牌。 |
| `--session-idle-timeout <sec>` | `0`（不超时） | 会话空闲多少秒后自动回收（终止其子进程、释放资源）。`0` 表示永不。**当前实现**：参数已解析但尚未启用自动回收；会话通过 `session.close` 或 `server.shutdown` 显式回收。 |

## 检索与查看

| 参数 | 默认 | 说明 |
|---|---|---|
| `--context-lines <n>` | `3` | 检索结果默认上下文行数；会话可用 `session.set_config` 覆盖。 |
| `--search-tasks <n>` | `4` | 检索 fan-out 并发宽度（`CacheSystem` 的 `num_tasks`），会话可用 `session.open` 覆盖。 |
| `--plugin-dir <path>` | `<exe>/plugins` | 语言插件目录（传给 `LangPluginManager`）。 |
| `--max-read-lines <n>` | `2000` | `file.read_lines` 单次 `max_lines` 上限（防止一次拉取过大）。 |
| `--max-read-bytes <n>` | `1048576` | `file.read_bytes` 单次 `max_bytes` 上限。 |

## 能力开关（安全默认关闭高风险功能）

| 参数 | 默认 | 说明 |
|---|---|---|
| `--allow-write` | 关 | 允许 `edit.*` 的 `apply:true` 真正写盘。关闭时 `apply:true` 降级为 dry-run 或返回 `path_denied`。 |
| `--allow-process` | 关 | 启用 `process.*` 命名空间。关闭时这些 method 返回 `unknown_method`。 |
| `--allow-shutdown` | 关 | 启用 `server.shutdown`。 |
| `--exec-allowlist <a,b,c>` | 空 | 逗号分隔的可执行文件白名单。非空时 `process.spawn` 的 `exec` 必须命中，否则 `path_denied`。空表示不限制（仅在 `--allow-process` 时有意义）。 |

## 二进制传输

| 参数 | 默认 | 说明 |
|---|---|---|
| `--inline-binary-limit <bytes>` | `262144` | `binary.read` 小于等于该值时内联返回，否则转引用形态分块拉取。 |
| `--binary-chunk-size <bytes>` | `65536` | 引用资源建议分块大小（`binary_ref.chunk_size`）。 |

## 诊断

| 参数 | 默认 | 说明 |
|---|---|---|
| `--log-level <level>` | `info` | `error` \| `warn` \| `info` \| `debug`。日志写 stderr，绝不污染 stdout。 |
| `--version` | — | 打印版本并退出。 |
| `--help` | — | 打印用法并退出。 |

## 示例

```bash
# 默认 stdio，只读检索/查看
indextools-server --stdio --root /path/to/repo

# 开放编辑写盘与受限子进程
indextools-server --stdio --root /path/to/repo \
  --allow-write --allow-process --exec-allowlist git,python3

# 调试
indextools-server --stdio --root . --log-level debug
```

## 输出契约

- stdout：只写协议响应（每行一条 JSON）。
- stderr：日志、诊断、启动横幅。
- 退出码：`0` 正常关停（EOF 或 `server.shutdown`）；非 `0` 表示启动失败（如插件目录不存在、`--root` 非法）。
