# aiapi

基于 Drogon 框架的 AI API 网关服务，提供 OpenAI 兼容的 Chat Completions 和 Responses API 接口。

## 功能特性

- ✅ OpenAI Chat Completions API 兼容（流式/非流式）
- ✅ OpenAI Responses API 兼容（流式/非流式，含 previous_response_id 续聊）
- ✅ 多 Provider 支持（可扩展工厂模式）
- ✅ 工具调用（Tool Calls）完整支持
- ✅ 工具调用桥接（XML Bridge）— 为不原生支持工具调用的通道提供桥接
- ✅ 工具调用验证（ToolCallValidator）— 支持 None/Relaxed/Strict 三种校验模式
- ✅ 参数形状规范化（normalizeToolCallArguments）— 自动修复常见参数格式问题
- ✅ 强制工具调用兜底（generateForcedToolCall）— tool_choice=required 场景
- ✅ 会话追踪（Hash / ZeroWidth 两种模式）
- ✅ 会话连续性决策（ContinuityResolver）
- ✅ 并发门控（SessionExecutionGate + CancellationToken + RAII Guard）
- ✅ 输出清洗（ClientOutputSanitizer）
- ✅ 严格客户端规则（Kilo-Code / RooCode 适配）
- ✅ 统一错误模型（Errors）+ 错误统计（ErrorStatsService）
- ✅ 账号池管理（自动注册、Token 刷新、类型检测、轮转）
- ✅ 渠道管理（多渠道、状态控制、并发限制）
- ✅ 服务状态监控 + Prometheus 指标导出
- ✅ 内置日志查看 API（文件列表、尾部读取、过滤）

## 架构概览

```
┌─────────────────────────────────────────────────────────────────┐
│                         HTTP 层                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                    AiApi Controller                      │    │
│  │  POST /chaynsapi/v1/chat/completions                    │    │
│  │  POST /chaynsapi/v1/responses                           │    │
│  │  GET  /chaynsapi/v1/models                              │    │
│  │  GET  /chaynsapi/v1/responses/{id}                      │    │
│  │  DELETE /chaynsapi/v1/responses/{id}                    │    │
│  │  + 账号管理 / 渠道管理 / 监控 / 日志 API                │    │
│  └─────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                        适配层                                    │
│  ┌─────────────────┐     ┌──────────────────────────────────┐  │
│  │ RequestAdapters │ ──▶ │      GenerationRequest           │  │
│  │ (Chat/Responses)│     │ (统一请求结构)                    │  │
│  └─────────────────┘     └──────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      生成编排层                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │               GenerationService                          │    │
│  │  - runGuarded()          (主入口，含并发门控)             │    │
│  │  - materializeSession()  (请求 → 会话)                   │    │
│  │  - executeProvider()     (调用上游)                       │    │
│  │  - emitResultEvents()    (结果处理 + 事件发送)            │    │
│  └─────────────────────────────────────────────────────────┘    │
│     │           │              │              │              │   │
│     ▼           ▼              ▼              ▼              ▼   │
│  ┌────────┐ ┌────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐  │
│  │ToolCall│ │Session │ │ToolCall  │ │Output    │ │Continuity│  │
│  │Bridge  │ │Manager │ │Validator │ │Sanitizer │ │Resolver  │  │
│  └────────┘ └────────┘ └──────────┘ └──────────┘ └──────────┘  │
│     │                                                           │
│     ▼                                                           │
│  ┌──────────────────┐                                           │
│  │SessionExecution  │ (并发门控 + CancellationToken)            │
│  │Gate              │                                           │
│  └──────────────────┘                                           │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Provider 层                                 │
│  ┌─────────────┐     ┌─────────────────────────────────────┐   │
│  │  ApiManager │ ──▶ │         APIinterface                │   │
│  │  (路由选择)  │     │  - generate()                       │   │
│  │  ApiFactory  │     │  - ProviderResult                   │   │
│  └─────────────┘     └─────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                       输出层                                     │
│  ┌───────────────┐  ┌───────────────┐  ┌───────────────────┐   │
│  │ IResponseSink │  │GenerationEvent│  │   HTTP Response   │   │
│  │  (接口)       │◀─│  (事件模型)    │──▶│  (JSON/SSE)       │   │
│  └───────────────┘  └───────────────┘  └───────────────────┘   │
│        │                                                         │
│        ├── ChatJsonSink      (Chat 非流式 JSON)                  │
│        ├── ChatSseSink       (Chat 流式 SSE)                     │
│        ├── ResponsesJsonSink (Responses 非流式 JSON)              │
│        ├── ResponsesSseSink  (Responses 流式 SSE)                │
│        ├── CollectorSink     (事件收集，内部用)                   │
│        └── NullSink          (丢弃输出，测试用)                   │
└─────────────────────────────────────────────────────────────────┘
```

## 项目结构

```
aiapi/
├── CMakeLists.txt                  # CMake 构建配置
├── Dockerfile                      # Docker 构建文件
├── config.example.json             # 配置文件模板
├── docker-compose.env.yml          # Docker Compose（环境变量方式）
├── docker-compose.volume.yml       # Docker Compose（卷挂载方式）
├── requirements.txt                # Python 依赖（登录/注册服务）
├── doc/                            # 文档目录
│   └── aiapi_callflow_and_api_examples.md  # 详细调用关系与接口样例
│
└── src/
    ├── main.cc                     # 程序入口
    ├── config.json                 # Drogon 运行配置
    │
    ├── controllers/                # HTTP 控制器
    │   ├── AiApi.h / AiApi.cc      # 主路由控制器（22 个 API 端点）
    │   └── sinks/                  # 输出 Sink 实现
    │       ├── ChatJsonSink.cpp    # Chat 非流式 JSON 输出
    │       ├── ChatSseSink.cpp     # Chat 流式 SSE 输出
    │       ├── ResponsesJsonSink.cpp # Responses 非流式 JSON 输出
    │       └── ResponsesSseSink.cpp  # Responses 流式 SSE 输出
    │
    ├── sessionManager/             # 核心业务逻辑
    │   ├── GenerationRequest.h     # 统一请求结构
    │   ├── GenerationEvent.h       # 统一事件模型
    │   ├── IResponseSink.h         # 输出通道接口
    │   ├── RequestAdapters.h/cpp   # HTTP 请求 → GenerationRequest 适配器
    │   ├── GenerationService.h/cpp # 生成编排服务（核心）
    │   ├── Session.h/cpp           # 会话管理 + ZeroWidth/Hash 追踪
    │   ├── ContinuityResolver.h    # 会话连续性决策器
    │   ├── ResponseIndex.h         # 响应存储索引（Responses API GET/DELETE）
    │   ├── SessionExecutionGate.h  # 并发门控（单例 + RAII Guard）
    │   ├── ToolCallBridge.h/cpp    # 工具调用桥接（Native / TextBridge）
    │   ├── XmlTagToolCallCodec.h/cpp # XML 格式工具调用编解码
    │   ├── ToolCallValidator.h/cpp # 工具调用 Schema 校验
    │   ├── ClientOutputSanitizer.h/cpp # 输出清洗
    │   └── Errors.h                # 统一错误模型
    │
    ├── apipoint/                   # Provider 抽象
    │   ├── APIinterface.h          # Provider 接口（generate / getModels）
    │   ├── ProviderResult.h        # Provider 结果结构
    │   └── chaynsapi/              # chayns Provider 实现
    │       └── chaynsapi.h/cpp
    │
    ├── apiManager/                 # Provider 管理
    │   ├── ApiFactory.h/cpp        # Provider 工厂
    │   └── ApiManager.h/cpp        # Provider 路由选择
    │
    ├── accountManager/             # 账号池管理
    │   └── accountManager.h/cpp    # 账号 CRUD + Token 刷新 + 自动注册
    │
    ├── channelManager/             # 渠道管理
    │   └── channelManager.h/cpp    # 渠道 CRUD + 状态控制
    │
    ├── dbManager/                  # 数据库管理
    │   ├── account/accountDbManager.h/cpp   # 账号持久化
    │   ├── channel/channelDbManager.h/cpp   # 渠道持久化
    │   └── metrics/                # 指标数据库管理
    │       ├── ErrorStatsDbManager.h/cpp    # 错误统计持久化
    │       └── StatusDbManager.h/cpp        # 服务状态持久化
    │
    ├── metrics/                    # 错误统计服务
    │   ├── ErrorStatsService.h     # 错误记录服务
    │   └── ErrorEvent.h            # 错误事件定义
    │
    └── tools/                      # 工具类
        └── ZeroWidthEncoder.h/cpp  # 零宽字符编码/解码
```

## 完整 API 端点清单

### AI 核心 API

| 方法 | 路径 | 功能 |
|------|------|------|
| POST | `/chaynsapi/v1/chat/completions` | Chat Completions（流式/非流式） |
| POST | `/chaynsapi/v1/responses` | Responses API（流式/非流式） |
| GET | `/chaynsapi/v1/responses/{id}` | 获取已创建的响应 |
| DELETE | `/chaynsapi/v1/responses/{id}` | 删除已创建的响应 |
| GET | `/chaynsapi/v1/models` | 获取可用模型列表 |

### 账号管理 API

| 方法 | 路径 | 功能 |
|------|------|------|
| POST | `/aichat/account/add` | 批量添加账号（支持对象/数组） |
| POST | `/aichat/account/delete` | 批量删除账号（含上游删除） |
| POST | `/aichat/account/update` | 批量更新账号信息 |
| POST | `/aichat/account/refresh` | 异步刷新所有账号 token + 类型 |
| POST | `/aichat/account/autoregister` | 自动注册新账号（最多 20 个/次） |
| GET | `/aichat/account/info` | 获取内存中的账号列表 |
| GET | `/aichat/account/dbinfo` | 获取数据库中的账号列表 |

### 渠道管理 API

| 方法 | 路径 | 功能 |
|------|------|------|
| POST | `/aichat/channel/add` | 批量添加渠道 |
| POST | `/aichat/channel/delete` | 批量删除渠道 |
| POST | `/aichat/channel/update` | 更新渠道配置 |
| POST | `/aichat/channel/updatestatus` | 更新渠道启用/禁用状态 |
| GET | `/aichat/channel/info` | 获取渠道列表 |

### 监控与日志 API

| 方法 | 路径 | 功能 |
|------|------|------|
| GET | `/aichat/metrics/requests/series` | 请求量时序统计 |
| GET | `/aichat/metrics/errors/series` | 错误量时序统计（多维过滤） |
| GET | `/aichat/metrics/errors/events` | 错误事件列表（分页） |
| GET | `/aichat/metrics/errors/events/{id}` | 错误事件详情 |
| GET | `/aichat/status/summary` | 服务状态概览 |
| GET | `/aichat/status/channels` | 渠道状态列表 |
| GET | `/aichat/status/models` | 模型状态列表 |
| GET | `/aichat/logs/list` | 日志文件列表 |
| GET | `/aichat/logs/tail` | 日志尾部读取（支持级别/关键词过滤） |

## 核心模块说明

### GenerationService（生成编排服务）

核心编排服务，管理整个生成流程：

```
runGuarded(req, sink, policy)
  ├─ materializeSession()         → GenerationRequest → session_st
  ├─ ContinuityResolver::resolve() → 会话连续性决策
  ├─ sessionManager.getOrCreateSession()
  └─ executeGuardedWithSession()
       ├─ ExecutionGuard(RAII)     → 获取并发锁
       ├─ transformRequestForToolBridge()  → 工具定义注入（Bridge 模式）
       ├─ executeProvider()        → 调用上游 AI
       ├─ emitResultEvents()       → 结果处理 + 事件发送
       │    ├─ sanitizeOutput()     → 文本清洗
       │    ├─ parseXmlToolCalls()  → XML 工具调用解析
       │    ├─ normalizeToolCallArguments() → 参数规范化
       │    ├─ ToolCallValidator    → Schema 校验 + 过滤
       │    ├─ applyStrictClientRules()    → 严格客户端规则
       │    └─ 零宽字符会话ID嵌入
       └─ coverSessionresponse()   → 会话上下文更新 + 转移
```

### GenerationEvent（统一事件模型）

| 事件类型 | 说明 | 关键数据 |
|----------|------|----------|
| `Started` | 生成开始 | responseId, model |
| `OutputTextDelta` | 文本增量（流式） | delta, index |
| `OutputTextDone` | 文本完成 | text, index |
| `ToolCallDone` | 工具调用完成 | id, name, arguments, index |
| `Usage` | Token 使用量 | inputTokens, outputTokens |
| `Completed` | 生成完成 | finishReason (stop/tool_calls) |
| `Error` | 错误 | code, message, detail |

### Tool Bridge 机制

为不支持原生 Tool Calls 的上游通道提供 XML 桥接：

**请求侧**：
1. 将工具定义编码为文本格式（支持 compact/full 两种模式）
2. 生成随机触发标记（如 `<Function_Ab1c_Start/>`）
3. 构建 `<tool_instructions>` 提示注入到 requestmessage

**响应侧**：
1. 通过触发标记定位 XML 块（防止误解析历史消息）
2. 使用 XmlTagToolCallCodec 解析 `<function_calls>/<function_call>` 结构
3. 参数形状规范化 + Schema 校验 + 降级策略

### 客户端适配

| 客户端 | 标识 | 特殊处理 |
|--------|------|----------|
| Kilo-Code | `Kilo-Code` | 严格模式：每次只能 1 个 tool call，纯文本自动包装为 attempt_completion |
| RooCode | `RooCode` | 同 Kilo-Code |
| Claude Code | `claudecode` | 零宽会话 ID 在 tool_calls 前单独发送 |
| 其他 | — | 宽松模式，不强制校验工具调用 |

### 会话追踪

| 模式 | 实现 | 说明 |
|------|------|------|
| Hash | 消息内容 SHA256 | 默认模式，基于 systemPrompt + messages 哈希 |
| ZeroWidth | 零宽字符嵌入 | 在助手回复中嵌入不可见的 sessionId |

### 并发门控

- **RejectConcurrent**：同一会话有请求在执行时，新请求返回 409 Conflict
- **CancelPrevious**：取消之前的请求，执行新请求
- 使用 RAII `ExecutionGuard` 自动管理生命周期

### 错误统计系统

错误按 4 个域分类：

| 域 | 说明 | 典型事件 |
|------|------|----------|
| `SESSION_GATE` | 会话并发门控 | 并发冲突、请求取消 |
| `UPSTREAM` | 上游 Provider | HTTP 错误、超时 |
| `TOOL_BRIDGE` | 工具桥接 | XML 未找到、校验过滤、降级、强制生成 |
| `INTERNAL` | 内部异常 | 运行时异常、未知错误 |

## API 使用示例

### Chat Completions API

```bash
# 非流式
curl -X POST "http://localhost:5555/chaynsapi/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "messages": [{"role": "user", "content": "Hello"}]
  }'

# 流式
curl -N -X POST "http://localhost:5555/chaynsapi/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "stream": true,
    "messages": [{"role": "user", "content": "Hello"}]
  }'
```

### Responses API

```bash
# 创建 Response
curl -X POST "http://localhost:5555/chaynsapi/v1/responses" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "input": "Hello"
  }'

# 续聊
curl -X POST "http://localhost:5555/chaynsapi/v1/responses" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "previous_response_id": "resp_abc123",
    "input": "Tell me more."
  }'

# 获取 Response
curl "http://localhost:5555/chaynsapi/v1/responses/{response_id}"

# 删除 Response
curl -X DELETE "http://localhost:5555/chaynsapi/v1/responses/{response_id}"
```

### 账号管理

```bash
# 添加账号
curl -X POST "http://localhost:5555/aichat/account/add" \
  -H "Content-Type: application/json" \
  -d '{
    "apiname": "chaynsapi",
    "username": "user@example.com",
    "password": "xxx"
  }'

# 自动注册 5 个账号
curl -X POST "http://localhost:5555/aichat/account/autoregister" \
  -H "Content-Type: application/json" \
  -d '{"apiname": "chaynsapi", "count": 5}'

# 刷新所有账号状态
curl -X POST "http://localhost:5555/aichat/account/refresh"
```

### 渠道管理

```bash
# 添加渠道
curl -X POST "http://localhost:5555/aichat/channel/add" \
  -H "Content-Type: application/json" \
  -d '[{
    "channelname": "main",
    "channeltype": "chaynsapi",
    "channelurl": "https://api.example.com",
    "channelkey": "sk-xxx",
    "maxconcurrent": 10,
    "supports_tool_calls": false
  }]'

# 获取渠道列表
curl "http://localhost:5555/aichat/channel/info"
```

### 监控

```bash
# 服务状态概览
curl "http://localhost:5555/aichat/status/summary"

# 错误时序统计（最近 24 小时）
curl "http://localhost:5555/aichat/metrics/errors/series"

# 日志尾部（过滤 ERROR 级别）
curl "http://localhost:5555/aichat/logs/tail?lines=100&level=ERROR"
```

## 构建与运行

### 依赖

- C++17 或更高版本
- Drogon 框架
- JsonCpp
- OpenSSL
- spdlog
- PostgreSQL（用于持久化）

### 本地构建

```bash
cd aiapi/src
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 运行

```bash
cd aiapi/src/build
./aiapi
```

服务默认监听 `0.0.0.0:5555`

### Docker 构建与运行

```bash
# 方式一：环境变量注入配置
docker compose -f docker-compose.env.yml up --build

# 方式二：卷挂载配置文件
cp config.example.json config.json
# 编辑 config.json 填入实际配置
docker compose -f docker-compose.volume.yml up --build
```

Docker 入口脚本支持：
- `CONFIG_JSON` 环境变量 → 直接覆盖配置文件
- `CUSTOM_CONFIG` 环境变量 → 使用 jq 合并到现有配置

## 配置说明

配置文件位于 `config.example.json`，主要配置项：

```json
{
  "listeners": [
    { "address": "0.0.0.0", "port": 5555 }
  ],
  "db_clients": [
    { "name": "aichatpg", "rdbms": "postgresql", "host": "...", ... }
  ],
  "app": {
    "number_of_threads": 4,
    "log": {
      "use_spdlog": true,
      "log_level": "DEBUG"
    },
    "cors": { "enabled": true, "allow_origins": ["*"] }
  },
  "plugins": [
    { "name": "drogon::plugin::PromExporter", "config": { "path": "/metrics" } },
    { "name": "drogon::plugin::AccessLogger" }
  ],
  "custom_config": {
    "session_tracking": {
      "mode": "zerowidth"
    },
    "tool_bridge": {
      "definition_mode": "compact",
      "include_descriptions": false,
      "max_description_chars": 160
    },
    "login_service_urls": [
      { "name": "chaynsapi", "url": "http://127.0.0.1:5557/aichat/chayns/login" }
    ],
    "regist_service_urls": [
      { "name": "chaynsapi", "url": "http://127.0.0.1:5557/aichat/chayns/autoregister" }
    ]
  }
}
```

### 关键配置项

| 配置路径 | 说明 | 可选值 |
|----------|------|--------|
| `custom_config.session_tracking.mode` | 会话追踪模式 | `hash` / `zerowidth` |
| `custom_config.tool_bridge.definition_mode` | 工具定义编码模式 | `compact` / `full` |
| `custom_config.tool_bridge.include_descriptions` | 是否包含工具描述 | `true` / `false` |
| `custom_config.tool_bridge.max_description_chars` | 描述截断长度 | 0-2000 |

## 错误码

| 错误码 | HTTP Status | 说明 |
|--------|-------------|------|
| BadRequest | 400 | 请求格式错误 |
| Unauthorized | 401 | 未授权 |
| Forbidden | 403 | 禁止访问 |
| NotFound | 404 | 资源不存在 |
| Conflict | 409 | 并发冲突（同一会话已有请求在执行） |
| RateLimited | 429 | 限流 |
| Timeout | 504 | 超时 |
| ProviderError | 502 | Provider 错误 |
| Internal | 500 | 内部错误 |
| Cancelled | 499 | 请求被取消 |

## 详细文档

- [调用关系图与接口样例](doc/aiapi_callflow_and_api_examples.md) — 详细的模块拆解、时序图、数据结构和 curl 示例

## 开发路线

- [x] Chat Completions API 基础功能
- [x] Responses API 基础功能（含 previous_response_id 续聊）
- [x] 流式输出支持（CollectorSink → SSE 分块传输）
- [x] 工具调用支持（原生 + Bridge）
- [x] 工具调用桥接（XML Bridge + 随机 Sentinel）
- [x] 工具调用验证（None/Relaxed/Strict + 降级策略）
- [x] 参数形状规范化（数组/别名/默认值）
- [x] 会话追踪（Hash/ZeroWidth + ContinuityResolver）
- [x] 并发门控（SessionExecutionGate + CancellationToken + RAII Guard）
- [x] 输出清洗（ClientOutputSanitizer）
- [x] 严格客户端规则（Kilo-Code / RooCode）
- [x] 统一错误模型（Errors）
- [x] 错误统计系统（ErrorStatsService + 4 域分类）
- [x] 账号池管理（自动注册 + Token 刷新 + 类型检测）
- [x] 渠道管理（CRUD + 状态控制 + supports_tool_calls）
- [x] 服务状态监控（Summary + Channels + Models）
- [x] 日志查看 API（文件列表 + 尾部读取 + 过滤）
- [x] Prometheus 指标导出
- [ ] 真正的流式 Provider 回调（当前为 CollectorSink 伪流式）
- [ ] 更多 Provider 实现（当前仅 chaynsapi）
- [ ] 完善单元测试

## License

MIT
