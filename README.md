# aiapi

基于 [Drogon](https://github.com/drogonframework/drogon) 的 AI API 网关服务，提供 OpenAI 兼容的 Chat Completions 与 Responses API，并统一管理 Provider、账号、渠道、工具调用、会话连续性和运行监控。

> 文档最后更新：2026-08-15

## 项目定位

`aiapi` 位于 AI 客户端与上游 AI Provider 之间，负责：

- 提供稳定的 OpenAI 风格 HTTP API；
- 将请求路由到不同 Provider 和渠道；
- 统一处理流式、非流式响应及错误；
- 适配不原生支持工具调用的上游服务；
- 管理账号池、Workspace、并发和限流；
- 提供健康检查、日志、指标和错误统计接口。

## 主要特性

- OpenAI Chat Completions API：流式与非流式；
- OpenAI Responses API：创建、查询、删除及 `previous_response_id` 续聊；
- 当前 Provider：`chaynsapi`、`retoolapi`；
- 工具调用、参数规范化、严格校验和强制工具调用兜底；
- XML Tool Bridge，兼容不支持原生函数调用的上游渠道；
- Codex XML 工具桥接；
- 会话追踪与会话连续性决策；
- 历史回放预算控制，按完整 turn 保留上下文；
- 账号池、自动注册、Token 刷新、轮转、备份与恢复；
- Retool Workspace 创建、验证、启停和资产管理；
- 多渠道配置、状态控制及并发限制；
- 管理接口认证与请求限流；
- `/health`、`/ready`、指标、错误统计和日志查看接口；
- RAII 并发门控、取消令牌和后台任务队列；
- CTest、集成测试以及 ASan、TSan、Coverage 构建配置。

## 架构

项目采用分层结构：

```text
src/
├── application/       应用服务与用例编排
├── domain/             领域模型、策略与端口
├── infrastructure/     Provider、持久化、HTTP、配置和运行时实现
├── platform/           通用 Result、错误模型等基础设施
├── runtime/            应用启动、关闭和生命周期管理
├── transport/          Drogon Controllers、Filters 和响应 Sink
└── test/                单元测试、集成测试和 Provider 固件
```

核心请求链路如下：

```text
HTTP Request
    ↓
Drogon Controller / Filter
    ↓
Request Adapter
    ↓
GenerationService
    ↓
Continuity / Tool Bridge / Session Gate
    ↓
Provider Registry → Provider
    ↓
JSON Sink 或 SSE Sink
    ↓
HTTP Response
```

## API 端点

### AI API

| 能力 | chaynsapi | retoolapi |
|---|---|---|
| Chat Completions | `POST /chaynsapi/v1/chat/completions` | `POST /retoolapi/v1/chat/completions` |
| Models | `GET /chaynsapi/v1/models` | `GET /retoolapi/v1/models` |
| 创建 Response | `POST /chaynsapi/v1/responses` | `POST /retoolapi/v1/responses` |
| 获取 Response | `GET /chaynsapi/v1/responses/{id}` | `GET /retoolapi/v1/responses/{id}` |
| 删除 Response | `DELETE /chaynsapi/v1/responses/{id}` | `DELETE /retoolapi/v1/responses/{id}` |

历史 Nexos 路由仍保留兼容边界，但实现已退役并返回 HTTP `410 Gone`：

- `/nexosapi/v1/chat/completions`
- `/nexosapi/v1/models`
- `/nexosapi/v1/responses/{id}`

### 管理 API

管理接口需要管理员认证，主要包括：

- 账号：`/aichat/account/*`
- 渠道：`/aichat/channel/*`
- Retool Workspace：`/aichat/retool/workspace/*`
- 指标与错误统计：`/aichat/metrics/*`
- 日志：`/aichat/logs/*`

### 健康检查

- `GET /health`：进程健康检查；
- `GET /ready`：服务就绪检查。

## 快速开始

### 依赖

- C++ 编译器，支持 C++17 或更高版本；
- CMake；
- Drogon 及其依赖；
- SQLite；
- OpenSSL、JsonCpp、uuid 等项目依赖。

具体依赖版本以构建环境和 `CMakeLists.txt` 为准。

### 配置

复制示例配置并根据实际环境修改：

```bash
cp config.example.json config.json
```

请至少检查以下配置：

- 服务监听地址和端口；
- 管理接口认证信息；
- SQLite 数据库路径；
- Provider 凭据与请求参数；
- 账号池和渠道配置；
- 日志、监控和限流配置。

不要将包含真实 Token、密码或 Cookie 的 `config.json` 提交到版本库。

### 本地构建

```bash
cmake -S . -B build
cmake --build build -j
```

可执行文件通常位于：

```text
build/aiapi
```

### 运行

```bash
./build/aiapi --config config.json
```

如果项目当前版本的启动参数不同，请以 `./build/aiapi --help` 及运行日志为准。

### Docker Compose

```bash
docker compose up --build
```

生产环境建议将配置文件和 SQLite 数据库通过卷挂载，并使用环境变量或密钥管理系统注入敏感信息。

## 测试与质量检查

运行完整测试：

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

也可以使用项目已有的独立构建目录：

```bash
ctest --test-dir build-asan --output-on-failure
ctest --test-dir build-tsan --output-on-failure
ctest --test-dir build-coverage --output-on-failure
```

线程消毒器相关脚本：

```bash
./tools/run-tsan.sh
```

提交代码前建议依次检查：

1. 普通构建成功；
2. CTest 全部通过；
3. 没有未提交的调试文件或敏感配置；
4. 结构和架构门禁通过；
5. 涉及生命周期、并发或 Provider 的修改已补充测试。

## 目录说明

| 路径 | 内容 |
|---|---|
| `src/application` | 账号、渠道、生成、监控和 Workspace 用例 |
| `src/domain` | 领域模型、策略、错误和端口定义 |
| `src/infrastructure/provider` | Provider 实现、Registry 和上游适配 |
| `src/infrastructure/persistence` | 数据库与持久化实现 |
| `src/transport/controllers` | HTTP 控制器、过滤器和 JSON/SSE Sink |
| `src/test` | 测试、Fixtures 和测试支持代码 |
| `tools` | 架构审计、TSan 等辅助工具 |
| `doc` / `docs` | 设计文档、开发计划和 ADR |
| `config.example.json` | 配置示例 |
| `Dockerfile` | 容器构建定义 |
| `docker-compose.yml` | 本地/部署编排示例 |

## 核心模块

### GenerationService

负责统一编排一次生成请求，包括请求适配、Provider 选择、会话连续性、历史回放、工具调用、取消处理以及输出事件生成。

### Tool Bridge

当上游不支持原生工具调用时，将工具定义编码为可识别的文本/XML 格式，并将上游返回的工具请求解析为统一的 Tool Call 事件。

相关能力包括：

- ToolDefinitionEncoder；
- ToolCallNormalizer；
- ToolCallValidator；
- ForcedToolCallGenerator；
- XML Tool Call Codec。

### Session Continuity

根据客户端、请求字段和历史上下文决定是否复用会话，并通过历史回放预算控制请求大小，避免截断单条消息造成上下文损坏。

### Response Sink

Chat 与 Responses API 分别提供 JSON Sink 和 SSE Sink，统一处理非流式响应、流式事件、错误和结束信号。

### 生命周期与并发

服务包含启动阶段数据库/HTTP 初始化、后台任务队列、会话执行门控、取消令牌以及关闭阶段的超时和线程回收处理。


## 请求处理流程

一次 AI 请求大致经过以下阶段：

1. **HTTP 接入**：Drogon Controller 接收请求，Filter 完成管理员认证、限流和基础校验。
2. **协议适配**：将 Chat Completions 或 Responses 请求转换为内部生成请求模型。
3. **渠道选择**：根据渠道状态、Provider 类型、模型和账号池策略选择可用上游。
4. **会话处理**：读取会话上下文，执行 `previous_response_id` 续聊和历史回放预算控制。
5. **工具处理**：编码工具定义，规范化工具调用参数，并在需要时通过 XML Bridge 兼容上游。
6. **生成执行**：由 `GenerationService` 驱动 Provider，处理重试、取消、并发门控和超时。
7. **事件转换**：将 Provider 输出转换为统一的 `GenerationEvent`。
8. **协议输出**：由 JSON Sink 或 SSE Sink 返回 OpenAI 兼容响应。
9. **监控记录**：记录请求状态、错误事件、耗时、渠道、模型和相关诊断信息。

## API 详细说明

### Chat Completions

```http
POST /chaynsapi/v1/chat/completions
POST /retoolapi/v1/chat/completions
Content-Type: application/json
Authorization: Bearer <token>
``

常用请求字段：

| 字段 | 说明 |
|---|---|
| `model` | 要使用的模型标识；具体可用值通过 `/v1/models` 查询 |
| `messages` | 对话消息数组 |
| `stream` | 是否使用 Server-Sent Events 流式响应 |
| `tools` | 可选的工具定义列表 |
| `tool_choice` | 工具选择策略，例如 `auto` 或强制指定工具 |
| `temperature` | 采样温度，是否生效取决于 Provider |
| `max_tokens` | 输出长度限制，最终限制可能受上游能力影响 |

非流式请求返回单个 JSON 对象；流式请求返回 `text/event-stream`，客户端应持续读取事件，直到收到结束事件或连接关闭。

### Responses API

```http
POST /chaynsapi/v1/responses
GET /chaynsapi/v1/responses/{id}
DELETE /chaynsapi/v1/responses/{id}
``

创建 Response 后，可以使用返回的 ID 查询或删除响应，并通过 `previous_response_id` 关联后续请求。服务会结合本地会话追踪和历史回放策略决定实际发送给 Provider 的上下文。

### Models API

```http
GET /chaynsapi/v1/models
GET /retoolapi/v1/models
``

Models API 返回当前渠道可用的模型目录。模型目录可能随 Provider、账号状态、渠道配置和上游能力变化，不应在客户端永久硬编码。

### 管理接口认证

以下接口属于管理面，通常需要通过 `AdminAuthFilter`：

```text
/aichat/account/*
/aichat/channel/*
/aichat/retool/workspace/*
/aichat/metrics/*
/aichat/logs/*
``

生产部署应将管理接口置于可信网络、反向代理或额外的访问控制之后，并避免在日志中输出认证信息和 Provider 凭据。

## 管理功能

### 账号管理

账号接口覆盖添加、删除、更新、刷新、自动注册、详情查询、备份信息和数据库信息：

```text
POST /aichat/account/add
POST /aichat/account/delete
POST /aichat/account/update
POST /aichat/account/refresh
POST /aichat/account/autoregister
GET  /aichat/account/info
GET  /aichat/account/backupinfo
GET  /aichat/account/dbinfo
GET  /aichat/account/settings
POST /aichat/account/settings
``

刷新和自动注册属于可能触发上游请求或后台任务的操作，应避免在高峰期重复调用。

### 渠道管理

```text
GET  /aichat/channel/list
POST /aichat/channel/add
POST /aichat/channel/update
POST /aichat/channel/delete
POST /aichat/channel/update-status
``

渠道状态会影响路由选择。禁用渠道后，新请求不应继续分配到该渠道，但正在执行的请求是否立即结束取决于其生命周期和 Provider 实现。

### Retool Workspace

Workspace 管理接口支持创建、更新、查询、列表、池状态、启用、禁用、删除和验证：

```text
POST /aichat/retool/workspace/create
POST /aichat/retool/workspace/upsert
GET  /aichat/retool/workspace/info
GET  /aichat/retool/workspace/list
GET  /aichat/retool/workspace/pool-status
POST /aichat/retool/workspace/enable
POST /aichat/retool/workspace/disable
POST /aichat/retool/workspace/delete
POST /aichat/retool/workspace/verify
``

## 监控与故障排查

### 指标和错误接口

```text
GET /aichat/metrics/requests/series
GET /aichat/metrics/errors/series
GET /aichat/metrics/errors/events
GET /aichat/metrics/errors/events/{id}
GET /aichat/metrics/status/summary
GET /aichat/metrics/status/channels
GET /aichat/metrics/status/models
GET /aichat/logs/list
GET /aichat/logs/tail
``

建议排查顺序：

1. 请求 `/health` 和 `/ready`，确认进程及依赖状态；
2. 查看状态概览，确认渠道和模型是否可用；
3. 查看错误时间序列，判断是否为突发或持续性问题；
4. 查询错误事件详情，定位 Provider、模型、账号和请求阶段；
5. 查看日志尾部，结合请求时间和错误 ID 进行关联；
6. 检查账号池、Workspace、限流和并发门控状态。

## 配置建议

配置文件中的敏感项包括 Provider Token、Cookie、管理员凭据和数据库连接信息。建议：

- 使用权限最小化的运行用户；
- 将配置文件权限限制为仅服务用户可读；
- 通过卷挂载或密钥系统注入生产配置；
- 将数据库、日志和备份目录持久化到独立卷；
- 轮换已写入日志或误提交到 Git 的凭据；
- 备份前确认备份文件同样受到访问控制保护；
- 升级前记录当前版本、配置摘要和数据库备份位置。

## 常见问题

### 服务能启动，但 `/ready` 失败

检查数据库初始化、配置文件路径、Provider 配置以及启动日志。`/health` 主要反映进程是否存活，而 `/ready` 还可能反映依赖和初始化状态。

### 请求返回模型不可用

先调用对应渠道的 Models API，再检查渠道状态、账号池状态和模型目录缓存。不同 Provider 的模型名称不一定相同。

### 流式响应中途断开

检查客户端是否正确处理 SSE、反向代理是否关闭缓冲、超时配置是否足够，以及 Provider 是否在生成过程中返回错误。服务端日志和错误事件接口可用于确认断开阶段。

### 工具调用无法解析

确认工具名称、JSON Schema 和参数格式有效。对于使用 XML Bridge 的渠道，应同时检查工具定义编码、上游原始文本和工具调用归一化日志。

### 上下文过长

减少历史消息、缩短工具输出或调整历史回放预算。服务会尽量按完整 turn 保留历史，避免简单截断造成消息结构不完整。

## Provider 数据归档与恢复

Provider 账号、Token、Workspace 和相关元数据属于运行数据。进行升级、迁移或批量操作前，应：

1. 停止写入或进入维护状态；
2. 备份 SQLite 数据库；
3. 归档 Provider 配置和必要的密钥材料；
4. 在测试环境验证恢复流程；
5. 恢复后检查 `/health`、`/ready`、账号池和渠道状态。

## 开发文档

仓库内提供以下文档：

- `doc/aiapi_callflow_and_api_examples.md`：调用流程与 API 示例；
- `doc/development-plan.md`：开发计划；
- `doc/error_stats_dev_plan.md`：错误统计开发计划；
- `doc/service_status_monitoring_design.md`：服务状态监控设计；
- `doc/optimization-report.md`：优化报告；
- `doc/session/`：会话连续性设计与开发文档；
- `doc/adr/`：架构决策记录；
- `docs/`：补充设计与待解决问题。

## 贡献规范

- 保持分层边界，不在 Controller 中堆积业务逻辑；
- 新增 Provider 功能时同步补充端口、用例、实现和测试；
- 所有外部请求、线程、定时器和数据库客户端都必须有明确的生命周期；
- 错误优先使用统一错误模型，不直接拼接不一致的错误响应；
- 修改 API 时同步更新测试、示例和文档；
- 不提交密钥、Token、Cookie、生产数据库或运行日志。

## License

当前仓库未在本 README 中声明具体开源许可证。使用、分发或二次开发前，请确认项目维护者提供的许可证和授权范围。
