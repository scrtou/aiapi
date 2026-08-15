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
