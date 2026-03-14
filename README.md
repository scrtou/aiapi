# aiapi

基于 Drogon 框架的 AI API 网关服务，提供 OpenAI 兼容的 Chat Completions 和 Responses API 接口。

## 功能特性

- ✅ OpenAI Chat Completions API 兼容（流式/非流式）
- ✅ OpenAI Responses API 兼容（流式/非流式，含 previous_response_id 续聊）
- ✅ 多 Provider 支持（可扩展工厂模式）
- ✅ 新增 Nexos Web Provider（`/nexosapi/v1/*`）
- ✅ OpenAI 兼容 Provider（`/openai/v1/*`）
- ✅ 工具调用（Tool Calls）完整支持
- ✅ 工具调用桥接（XML Bridge）— 为不原生支持工具调用的通道提供桥接
- ✅ 工具调用验证（ToolCallValidator）— 支持 None/Relaxed/Strict 三种校验模式
- ✅ 参数形状规范化（ToolCallNormalizer）— 自动修复常见参数格式问题
- ✅ 工具定义编码（ToolDefinitionEncoder）— compact/full 两种模式
- ✅ 强制工具调用兜底（ForcedToolCallGenerator）— tool_choice=required 场景
- ✅ 严格客户端规则（StrictClientRules）— Kilo-Code / RooCode 适配
- ✅ 会话追踪（Hash / ZeroWidth 两种模式）
- ✅ 会话连续性决策（ContinuityResolver + TextExtractor）
- ✅ 响应索引（ResponseIndex）— Responses API GET/DELETE 支持
- ✅ 并发门控（SessionExecutionGate + CancellationToken + RAII Guard）
- ✅ 输出清洗（ClientOutputSanitizer）
- ✅ 统一错误模型（Errors）+ 错误统计（ErrorStatsService + ErrorStatsConfig）
- ✅ 账号池管理（自动注册、Token 刷新、类型检测、轮转、备份）
- ✅ 渠道管理（多渠道、状态控制、并发限制）
- ✅ 服务状态监控 + Prometheus 指标导出
- ✅ 内置日志查看 API（文件列表、尾部读取、过滤）
- ✅ 管理接口认证（AdminAuthFilter）
- ✅ 请求限流（RateLimitFilter）
- ✅ 配置校验（ConfigValidator）
- ✅ 后台任务队列（BackgroundTaskQueue）
- ✅ 健康检查端点（/health + /ready）
- ✅ 完善的单元测试（14 个测试文件）

## 架构概览

```
┌─────────────────────────────────────────────────────────────────┐
│                         HTTP 层                                  │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │              Controllers + Filters                      │    │
│  │  AiApiController     — AI 核心 API 路由                 │    │
│  │  AccountController   — 账号管理 API                     │    │
│  │  ChannelController   — 渠道管理 API                     │    │
│  │  MetricsController   — 监控指标 API                     │    │
│  │  LogController       — 日志查看 API                     │    │
│  │  HealthController    — 健康检查 API                     │    │
│  │  AdminAuthFilter     — 管理接口认证                     │    │
│  │  RateLimitFilter     — 请求限流                         │    │
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
│        │                                                        │
│        ├── chaynsapi (Chayns AI Provider)                       │
│        ├── nexosapi  (Nexos Web Provider)                       │
│        └── openai    (OpenAI 兼容 Provider)                     │
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
├── CMakeLists.txt                  # CMake 构建配置（根目录）
├── Dockerfile                      # Docker 构建文件
├── config.example.json             # 配置文件模板（JSON）
├── config.sqlite.example.json      # SQLite 配置模板
├── docker-compose.env.yml          # Docker Compose（环境变量方式）
├── docker-compose.volume.yml       # Docker Compose（卷挂载方式）
├── requirements.txt                # Python 依赖（登录/注册服务）
├── doc/                            # 文档目录
│   └── aiapi_callflow_and_api_examples.md  # 详细调用关系与接口样例
│
└── src/
    ├── CMakeLists.txt              # CMake 构建配置（源码）
    ├── main.cc                     # 程序入口
    ├── config.yaml                 # Drogon YAML 运行配置
    │
    ├── controllers/                # HTTP 控制器 + 过滤器
    │   ├── AiApiController.h/cc    # AI 核心 API 路由控制器
    │   ├── AccountController.h/cc  # 账号管理 API 控制器
    │   ├── ChannelController.h/cc  # 渠道管理 API 控制器
    │   ├── MetricsController.h/cc  # 监控指标 API 控制器
    │   ├── LogController.h/cc      # 日志查看 API 控制器
    │   ├── HealthController.h/cc   # 健康检查 API 控制器
    │   ├── ControllerUtils.h       # 控制器公共工具
    │   ├── AdminAuthFilter.h       # 管理接口 Bearer Token 认证过滤器
    │   ├── RateLimitFilter.h       # 请求限流过滤器（令牌桶）
    │   └── sinks/                  # 输出 Sink 实现
    │       ├── ChatJsonSink.h/cpp      # Chat 非流式 JSON 输出
    │       ├── ChatSseSink.h/cpp       # Chat 流式 SSE 输出
    │       ├── ResponsesJsonSink.h/cpp # Responses 非流式 JSON 输出
    │       └── ResponsesSseSink.h/cpp  # Responses 流式 SSE 输出
    │
    ├── sessionManager/             # 核心业务逻辑（分层组织）
    │   ├── README.md               # sessionManager 模块文档
    │   │
    │   ├── contracts/              # 接口契约与数据结构
    │   │   ├── README.md           # 契约层文档
    │   │   ├── GenerationRequest.h # 统一请求结构
    │   │   ├── GenerationEvent.h   # 统一事件模型
    │   │   └── IResponseSink.h     # 输出通道接口
    │   │
    │   ├── core/                   # 核心服务
    │   │   ├── README.md           # 核心层文档
    │   │   ├── GenerationService.h/cpp              # 生成编排服务（主入口）
    │   │   ├── GenerationServiceEmitAndToolBridge.cpp # 事件发送 + 工具桥接逻辑
    │   │   ├── RequestAdapters.h/cpp   # HTTP 请求 → GenerationRequest 适配器
    │   │   ├── Session.h/cpp           # 会话管理 + ZeroWidth/Hash 追踪
    │   │   ├── SessionExecutionGate.h  # 并发门控（单例 + RAII Guard）
    │   │   ├── ClientOutputSanitizer.h/cpp # 输出清洗
    │   │   └── Errors.h                # 统一错误模型
    │   │
    │   ├── continuity/             # 会话连续性
    │   │   ├── README.md           # 连续性模块文档
    │   │   ├── ContinuityResolver.h/cpp # 会话连续性决策器
    │   │   ├── ResponseIndex.h/cpp      # 响应存储索引（Responses API GET/DELETE）
    │   │   └── TextExtractor.h/cpp      # 文本提取工具
    │   │
    │   └── tooling/                # 工具调用相关
    │       ├── README.md           # 工具调用模块文档
    │       ├── ToolCallBridge.h/cpp         # 工具调用桥接（Native / TextBridge）
    │       ├── ToolDefinitionEncoder.h/cpp  # 工具定义编码（compact/full）
    │       ├── XmlTagToolCallCodec.h/cpp    # XML 格式工具调用编解码
    │       ├── ToolCallValidator.h/cpp      # 工具调用 Schema 校验
    │       ├── ToolCallNormalizer.h/cpp     # 参数形状规范化
    │       ├── ForcedToolCallGenerator.h/cpp # 强制工具调用兜底生成
    │       ├── StrictClientRules.h/cpp      # 严格客户端规则
    │       └── BridgeHelpers.h/cpp          # 桥接辅助函数
    │
    ├── apipoint/                   # Provider 抽象与实现
    │   ├── APIinterface.h          # Provider 接口（generate / getModels）
    │   ├── ProviderResult.h        # Provider 结果结构
    │   ├── chaynsapi/              # Chayns Provider 实现
    │   │   └── chaynsapi.h/cpp
    │   ├── nexosapi/               # Nexos Web Provider 实现
    │   │   └── nexosapi.h/cpp
    │   └── openai/                 # OpenAI 兼容 Provider 实现
    │       └── OpenAiProvider.h/cpp
    │
    ├── apiManager/                 # Provider 管理
    │   ├── Apicomn.h               # API 公共定义
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
    │   ├── DbType.h                # 数据库类型枚举（PostgreSQL/MySQL/SQLite3）
    │   ├── account/
    │   │   ├── accountDbManager.h/cpp       # 账号持久化
    │   │   └── accountBackupDbManager.h/cpp # 账号备份持久化
    │   ├── channel/
    │   │   └── channelDbManager.h/cpp       # 渠道持久化
    │   ├── config/
    │   │   └── ConfigDbManager.h/cpp        # 配置持久化（app_config 表）
    │   └── metrics/
    │       ├── ErrorStatsDbManager.h/cpp    # 错误统计持久化
    │       └── StatusDbManager.h/cpp        # 服务状态持久化
    │
    ├── metrics/                    # 错误统计服务
    │   ├── ErrorEvent.h            # 错误事件定义
    │   ├── ErrorStatsConfig.h/cpp  # 错误统计配置
    │   └── ErrorStatsService.h/cpp # 错误记录服务
    │
    ├── models/                     # 数据模型
    │   └── model.json              # 模型定义文件
    │
    ├── tools/                      # 工具类
    │   ├── ZeroWidthEncoder.h/cpp  # 零宽字符编码/解码
    │   └── accountlogin/           # 账号登录自动化
    │       ├── login_client.cpp    # C++ 登录客户端
    │       ├── loginlocal.py       # 本地登录脚本
    │       ├── loginremote.py      # 远程登录脚本
    │       ├── chayns-login.service # systemd 服务文件
    │       └── test.py             # 登录测试脚本
    │
    ├── utils/                      # 通用工具
    │   ├── BackgroundTaskQueue.h   # 后台任务队列
    │   └── ConfigValidator.h/cpp   # 配置校验器
    │
    └── test/                       # 单元测试
        ├── CMakeLists.txt          # 测试构建配置
        ├── test_main.cc            # 测试入口
        ├── test_continuity_resolver.cpp     # ContinuityResolver 测试
        ├── test_error_event.cpp             # ErrorEvent 测试
        ├── test_error_stats_config.cpp      # ErrorStatsConfig 测试
        ├── test_forced_tool_call.cpp        # ForcedToolCallGenerator 测试
        ├── test_generation_service_emit.cpp # GenerationService emit 测试
        ├── test_normalize_tool_args.cpp     # ToolCallNormalizer 测试
        ├── test_request_adapters.cpp        # RequestAdapters 测试
        ├── test_response_index.cpp          # ResponseIndex 测试
        ├── test_sinks.cpp                   # Sink 输出测试
        ├── test_strict_client_rules.cpp     # StrictClientRules 测试
        ├── test_tool_call_validator.cpp      # ToolCallValidator 测试
        └── test_xml_tool_call_codec.cpp     # XmlTagToolCallCodec 测试
```

## 完整 API 端点清单

### AI 核心 API（AiApiController）

| 方法 | 路径 | 功能 |
|------|------|------|
| POST | `/chaynsapi/v1/chat/completions` | Chat Completions（流式/非流式） |
| POST | `/chaynsapi/v1/responses` | Responses API（流式/非流式） |
| GET | `/chaynsapi/v1/responses/{id}` | 获取已创建的响应 |
| DELETE | `/chaynsapi/v1/responses/{id}` | 删除已创建的响应 |
| GET | `/chaynsapi/v1/models` | 获取可用模型列表 |
| POST | `/nexosapi/v1/chat/completions` | Nexos Web Chat → OpenAI Chat Completions |
| POST | `/nexosapi/v1/responses` | Nexos Web Chat → OpenAI Responses |
| GET | `/nexosapi/v1/responses/{id}` | 获取已创建的 Nexos 响应 |
| DELETE | `/nexosapi/v1/responses/{id}` | 删除已创建的 Nexos 响应 |
| GET | `/nexosapi/v1/models` | 获取 Nexos 可用模型列表 |
| GET | `/nexosapi/v1/account/quota` | 获取 Nexos 账号订阅/额度信息 |

### Nexos Provider 说明

- `nexosapi` 不再从配置文件读取 `cookies/default_model/default_handler_id/model_mapping/models`
- **账号 cookies 来自账号管理**：请通过 `/aichat/account/add` 添加 `apiName=nexosapi` 的账号，并把完整 cookies 放到 `authToken`
- **模型列表实时获取**：每次调用 `/nexosapi/v1/models` 或聊天请求时，都会从 Nexos `chat.data` 实时解析当前账号可用模型

### 账号管理 API（AccountController）

| 方法 | 路径 | 功能 |
|------|------|------|
| POST | `/aichat/account/add` | 批量添加账号（支持对象/数组） |
| POST | `/aichat/account/delete` | 批量删除账号（含上游删除） |
| POST | `/aichat/account/update` | 批量更新账号信息 |
| POST | `/aichat/account/refresh` | 异步刷新所有账号 token + 类型 |
| POST | `/aichat/account/autoregister` | 自动注册新账号（最多 20 个/次） |
| GET | `/aichat/account/info` | 获取内存中的账号列表 |
| GET | `/aichat/account/dbinfo` | 获取数据库中的账号列表 |

### 渠道管理 API（ChannelController）

| 方法 | 路径 | 功能 |
|------|------|------|
| POST | `/aichat/channel/add` | 批量添加渠道 |
| POST | `/aichat/channel/delete` | 批量删除渠道 |
| POST | `/aichat/channel/update` | 更新渠道配置 |
| POST | `/aichat/channel/updatestatus` | 更新渠道启用/禁用状态 |
| GET | `/aichat/channel/info` | 获取渠道列表 |

### 监控与日志 API（MetricsController + LogController）

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

### 健康检查 API（HealthController）

| 方法 | 路径 | 功能 |
|------|------|------|
| GET | `/health` | 返回服务状态、版本、运行时长 |
| GET | `/ready` | 检查数据库、Provider、账号池可用性（依赖不足时返回 503） |

## 核心模块说明

### GenerationService（生成编排服务）

核心编排服务，管理整个生成流程。代码拆分为两个 .cpp 文件：
- `GenerationService.cpp` — 主流程（runGuarded / materializeSession / executeProvider）
- `GenerationServiceEmitAndToolBridge.cpp` — 事件发送 + 工具桥接逻辑

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
       │    ├─ ToolCallNormalizer   → 参数规范化
       │    ├─ ToolCallValidator    → Schema 校验 + 过滤
       │    ├─ StrictClientRules    → 严格客户端规则
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

### Tool Bridge 机制（tooling/ 模块）

为不支持原生 Tool Calls 的上游通道提供 XML 桥接，模块已拆分为独立组件：

| 组件 | 文件 | 职责 |
|------|------|------|
| ToolCallBridge | `ToolCallBridge.h/cpp` | 桥接主逻辑（请求注入 + 响应解析） |
| ToolDefinitionEncoder | `ToolDefinitionEncoder.h/cpp` | 工具定义编码（compact/full 模式） |
| XmlTagToolCallCodec | `XmlTagToolCallCodec.h/cpp` | XML 格式工具调用编解码 |
| ToolCallValidator | `ToolCallValidator.h/cpp` | Schema 校验（None/Relaxed/Strict） |
| ToolCallNormalizer | `ToolCallNormalizer.h/cpp` | 参数形状规范化（数组/别名/默认值） |
| ForcedToolCallGenerator | `ForcedToolCallGenerator.h/cpp` | tool_choice=required 兜底生成 |
| StrictClientRules | `StrictClientRules.h/cpp` | Kilo-Code/RooCode 严格模式适配 |
| BridgeHelpers | `BridgeHelpers.h/cpp` | 桥接辅助函数 |

**请求侧**：
1. ToolDefinitionEncoder 将工具定义编码为文本格式
2. 生成随机触发标记（如 `<Function_Ab1c_Start/>`）
3. 构建 `<tool_instructions>` 提示注入到 request message

**响应侧**：
1. 通过触发标记定位 XML 块（防止误解析历史消息）
2. XmlTagToolCallCodec 解析 `<function_calls>/<function_call>` 结构
3. ToolCallNormalizer 参数规范化 + ToolCallValidator Schema 校验 + 降级策略

### 会话连续性（continuity/ 模块）

| 组件 | 职责 |
|------|------|
| ContinuityResolver | 决策当前请求是否属于已有会话的延续 |
| ResponseIndex | 响应存储索引，支持 Responses API 的 GET/DELETE 操作 |
| TextExtractor | 从复杂消息结构中提取纯文本内容 |

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

配置通过 `ErrorStatsConfig` 管理，支持运行时调整保留策略。

### HTTP 过滤器

| 过滤器 | 作用范围 | 说明 |
|--------|----------|------|
| AdminAuthFilter | `/aichat/*` | Bearer Token 认证，`admin_api_key` 为空时跳过（向后兼容） |
| RateLimitFilter | AI API 端点 | 令牌桶限流，可配置 `requests_per_second` 和 `burst` |

## API 使用示例

### Chat Completions API

```bash
# 非流式
curl -X POST "http://localhost:55555/chaynsapi/v1/chat/completions" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "messages": [{"role": "user", "content": "Hello"}]
  }'

# 流式
curl -N -X POST "http://localhost:55555/chaynsapi/v1/chat/completions" \
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
curl -X POST "http://localhost:55555/chaynsapi/v1/responses" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "input": "Hello"
  }'

# 续聊
curl -X POST "http://localhost:55555/chaynsapi/v1/responses" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "previous_response_id": "resp_abc123",
    "input": "Tell me more."
  }'

# 获取 Response
curl "http://localhost:55555/chaynsapi/v1/responses/{response_id}"

# 删除 Response
curl -X DELETE "http://localhost:55555/chaynsapi/v1/responses/{response_id}"
```

### 账号管理

```bash
# 添加账号
curl -X POST "http://localhost:55555/aichat/account/add" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR_ADMIN_KEY" \
  -d '{
    "apiname": "chaynsapi",
    "username": "user@example.com",
    "password": "xxx"
  }'

# 自动注册 5 个账号
curl -X POST "http://localhost:55555/aichat/account/autoregister" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR_ADMIN_KEY" \
  -d '{"apiname": "chaynsapi", "count": 5}'

# 刷新所有账号状态
curl -X POST "http://localhost:55555/aichat/account/refresh" \
  -H "Authorization: Bearer YOUR_ADMIN_KEY"
```

### 渠道管理

```bash
# 添加渠道
curl -X POST "http://localhost:55555/aichat/channel/add" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR_ADMIN_KEY" \
  -d '[{
    "channelname": "main",
    "channeltype": "chaynsapi",
    "channelurl": "https://api.example.com",
    "channelkey": "sk-xxx",
    "maxconcurrent": 10,
    "supports_tool_calls": false
  }]'

# 获取渠道列表
curl "http://localhost:55555/aichat/channel/info" \
  -H "Authorization: Bearer YOUR_ADMIN_KEY"
```

### 监控

```bash
# 服务状态概览
curl "http://localhost:55555/aichat/status/summary" \
  -H "Authorization: Bearer YOUR_ADMIN_KEY"

# 错误时序统计（最近 24 小时）
curl "http://localhost:55555/aichat/metrics/errors/series" \
  -H "Authorization: Bearer YOUR_ADMIN_KEY"

# 日志尾部（过滤 ERROR 级别）
curl "http://localhost:55555/aichat/logs/tail?lines=100&level=ERROR" \
  -H "Authorization: Bearer YOUR_ADMIN_KEY"

# 健康检查
curl "http://localhost:55555/health"
curl "http://localhost:55555/ready"

# Prometheus 指标
curl "http://localhost:55555/metrics"
```

## 构建与运行

### 依赖

- C++17 或更高版本
- Drogon 框架
- JsonCpp
- OpenSSL
- spdlog
- PostgreSQL / MySQL / SQLite3（可选，通过 `dbtype` 配置切换）

### 本地构建

```bash
cd aiapi/src
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 运行测试

```bash
cd aiapi/src/build
ctest --output-on-failure
# 或直接运行测试可执行文件
./test/aiapi_test
```

### 运行

```bash
cd aiapi/src/build
./aiapi
```

服务默认监听 `0.0.0.0:55555`

### Docker 构建与运行

```bash
# 方式一：环境变量注入配置
docker compose -f docker-compose.env.yml up --build aiapi

# 方式二：卷挂载配置文件
cp config.example.json config.json
# 编辑 config.json 填入实际配置
docker compose -f docker-compose.volume.yml up --build aiapi

# 方式三：SQLite + 卷挂载持久化
cp config.sqlite.example.json config.json
mkdir -p data logs cores
# config.json 中默认使用 ./data/aiapi.db，对应宿主机 ./data/aiapi.db
docker compose -f docker-compose.volume.yml up --build aiapi
```

`docker-compose.env.yml` 与 `docker-compose.volume.yml` 中的服务名、镜像名、容器名均统一为 `aiapi`。

Docker 入口脚本支持：
- `CONFIG_JSON` 环境变量 → 直接覆盖配置文件
- `CUSTOM_CONFIG` 环境变量 → 使用 jq 合并到现有配置

## 配置说明

配置文件位于 `config.example.json`，主要配置项：

```json
{
  "listeners": [
    { "address": "0.0.0.0", "port": 55555 }
  ],
  "db_clients": [
    { "name": "aichatpg", "rdbms": "postgresql", "host": "...", "...": "..." }
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
    "dbtype": "sqlite3",
    "admin_api_key": "",
    "session_tracking": {
      "mode": "zerowidth"
    },
    "tool_bridge": {
      "definition_mode": "compact",
      "include_descriptions": false,
      "max_description_chars": 5000,
      "trigger_random_length": 8,
      "strict_sentinel": true
    },
    "login_service_urls": [
      { "name": "chaynsapi", "url": "http://login-service:8004/api/v1/logins" }
    ],
    "regist_service_urls": [
      { "name": "chaynsapi", "url": "http://orchestrator-service:8000/api/v1/workflows/register-and-login" }
    ]
  }
}
```

### 账号自动化策略

账号自动化策略默认从 `custom_config.account_automation` 提供初始值；运行时优先从数据库配置表 `app_config` 读取，若表中缺失配置项则自动写入默认值。

- `auto_delete_enabled`：是否自动删除过期的 free 账号
- `delete_after_days`：账号创建超过多少天后删除，默认 `6`
- `auto_register_enabled`：当渠道账号数量不足时，是否自动补注册账号

### 关键配置项

| 配置路径 | 说明 | 可选值 |
|----------|------|--------|
| `custom_config.dbtype` | 数据库类型 | `postgresql` / `mysql` / `sqlite3` |
| `custom_config.admin_api_key` | 管理接口 Bearer Key（为空则兼容放行并告警） | 任意非空字符串 |
| `custom_config.session_tracking.mode` | 会话追踪模式 | `hash` / `zerowidth` |
| `custom_config.tool_bridge.definition_mode` | 工具定义编码模式 | `compact` / `full` |
| `custom_config.tool_bridge.include_descriptions` | 是否包含工具描述 | `true` / `false` |
| `custom_config.tool_bridge.max_description_chars` | 描述截断长度 | 0-5000 |
| `custom_config.tool_bridge.trigger_random_length` | 触发标记随机长度 | 6-12 |
| `custom_config.tool_bridge.strict_sentinel` | 严格哨兵模式（全局默认） | `true` / `false` |
| `custom_config.tool_bridge.strict_sentinel_by_channel` | 按渠道覆盖严格哨兵 | `{ "channel": bool }` |
| `custom_config.tool_bridge.strict_sentinel_by_model` | 按模型覆盖严格哨兵 | `{ "model": bool }` |
| `custom_config.tool_bridge.rewrite_user_input_conflicts` | 是否改写用户输入中的冲突指令 | `true` / `false` |
| `custom_config.rate_limit.enabled` | AI 接口限流开关 | `true` / `false` |
| `custom_config.rate_limit.requests_per_second` | 每秒令牌补充速率 | 正整数 |
| `custom_config.rate_limit.burst` | 瞬时突发上限 | 正整数 |
| `custom_config.response_index.max_entries` | Responses 索引最大内存条目数 | 正整数 |
| `custom_config.response_index.max_age_hours` | Responses 索引过期时间（小时） | 正整数 |
| `custom_config.response_index.cleanup_interval_minutes` | 索引清理周期（分钟） | 正整数 |
| `custom_config.providers.openai` | OpenAI 兼容 Provider 配置 | `api_key` / `base_url` / `default_model` |
| `custom_config.providers.nexos` | Nexos Provider 配置 | `base_url` |
| `custom_config.upstream_error_texts` | 上游错误文本匹配列表 | 字符串数组 |
| `custom_config.cors.allowed_origins` | CORS 白名单 | 字符串数组 |

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
- [sessionManager 模块文档](src/sessionManager/README.md) — 会话管理核心模块说明
- [contracts 层文档](src/sessionManager/contracts/README.md) — 接口契约说明
- [core 层文档](src/sessionManager/core/README.md) — 核心服务说明
- [continuity 模块文档](src/sessionManager/continuity/README.md) — 会话连续性模块说明
- [tooling 模块文档](src/sessionManager/tooling/README.md) — 工具调用模块说明

## 单元测试

项目包含 14 个测试文件，覆盖核心模块：

| 测试文件 | 覆盖模块 |
|----------|----------|
| `test_request_adapters.cpp` | HTTP 请求适配器 |
| `test_xml_tool_call_codec.cpp` | XML 工具调用编解码 |
| `test_tool_call_validator.cpp` | 工具调用 Schema 校验 |
| `test_normalize_tool_args.cpp` | 参数形状规范化 |
| `test_forced_tool_call.cpp` | 强制工具调用兜底 |
| `test_strict_client_rules.cpp` | 严格客户端规则 |
| `test_sinks.cpp` | 输出 Sink |
| `test_generation_service_emit.cpp` | GenerationService 事件发送 |
| `test_continuity_resolver.cpp` | 会话连续性决策 |
| `test_response_index.cpp` | 响应索引 |
| `test_error_event.cpp` | 错误事件模型 |
| `test_error_stats_config.cpp` | 错误统计配置 |

## 开发路线

- [x] Chat Completions API 基础功能
- [x] Responses API 基础功能（含 previous_response_id 续聊）
- [x] 流式输出支持（CollectorSink → SSE 分块传输）
- [x] 工具调用支持（原生 + Bridge）
- [x] 工具调用桥接（XML Bridge + 随机 Sentinel）
- [x] 工具调用验证（None/Relaxed/Strict + 降级策略）
- [x] 参数形状规范化（ToolCallNormalizer：数组/别名/默认值）
- [x] 工具定义编码（ToolDefinitionEncoder：compact/full）
- [x] 强制工具调用兜底（ForcedToolCallGenerator）
- [x] 严格客户端规则（StrictClientRules：Kilo-Code / RooCode）
- [x] 会话追踪（Hash/ZeroWidth + ContinuityResolver + TextExtractor）
- [x] 并发门控（SessionExecutionGate + CancellationToken + RAII Guard）
- [x] 输出清洗（ClientOutputSanitizer）
- [x] 统一错误模型（Errors）
- [x] 错误统计系统（ErrorStatsService + ErrorStatsConfig + 4 域分类）
- [x] 账号池管理（自动注册 + Token 刷新 + 类型检测 + 备份）
- [x] 渠道管理（CRUD + 状态控制 + supports_tool_calls）
- [x] 服务状态监控（Summary + Channels + Models）
- [x] 日志查看 API（文件列表 + 尾部读取 + 过滤）
- [x] Prometheus 指标导出
- [x] 增量流式响应（AsyncStreamResponse + SSE 实时推送）
- [x] 多 Provider（chaynsapi + nexosapi + OpenAI 兼容）
- [x] HTTP 过滤器（AdminAuthFilter + RateLimitFilter）
- [x] 健康检查端点（/health + /ready）
- [x] 配置校验（ConfigValidator）
- [x] 后台任务队列（BackgroundTaskQueue）
- [x] 控制器拆分（6 个独立控制器）
- [x] sessionManager 分层重构（contracts / core / continuity / tooling）
- [x] 核心单元测试（14 个测试文件覆盖关键模块）

## License

MIT
