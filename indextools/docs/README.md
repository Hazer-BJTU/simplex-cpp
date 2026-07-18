# indextools 服务端文档

本目录描述 indextools 服务端第一阶段（stdio 交互模式）的对外契约与设计。

## 文档索引

- [protocol.md](protocol.md) — 传输封装、请求/响应 envelope、错误模型、`id`/`token` 语义、路径安全约定。
- [server.md](server.md) — 服务端可执行文件的启动参数与运行模式。
- [commands.md](commands.md) — 每一个支持命令的功能、参数与返回 schema。
- [schemas.md](schemas.md) — 公共返回结构（`DisplayBlock[]`、`ProcessReport[]`、二进制块）的权威定义。
- [architecture.md](architecture.md) — 第一阶段内部架构与设计（分层、并发模型、各服务、已知限制、客户端待决问题）。英文。

## 阶段目标

第一阶段实现一个 **stdio JSON 工具服务端**：

- 以行分隔 JSON（NDJSON）在 stdin/stdout 上收发。
- 支持基于 `token` 的会话隔离（检索缓存、子进程、二进制资源、会话配置互不干扰）。
- 覆盖检索、查看、编辑（默认 dry-run）、子进程管理、二进制传输五类命令。
- 命令协议与传输层解耦，后续可无改动地复用于 TCP / WebSocket。

> 约定：本文档中出现的 method、参数名、字段名均为 **规范契约**。实现必须与此一致；若需变更，应先更新本目录再改代码。
