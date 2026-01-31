# aiapi - AI API Gateway Service

AI API 网关服务，基于 **Drogon (C++ Web 框架)** 构建，提供 OpenAI 兼容的 API 接口。支持多种 AI 提供商的统一接入、会话管理、工具调用桥接等高级功能。

## ✨ 特性

- **OpenAI 兼容接口**：支持 Chat Completions API 和 Responses API
- **多会话追踪模式**：Hash 模式（内容哈希）和 ZeroWidth 模式（零宽字符嵌入）
- **工具调用桥接**：为不支持原生 tool calls 的通道提供 XML 格式转换
- **统一请求适配**：`RequestAdapters` 将不同 API 格式统一为 `GenerationRequest`
- **并发门控**：`ExecutionGuard` 实现会话级并发控制
- **多渠道管理**：支持多上游 Provider 的负载均衡
- **账户管理**：内置账户认证与管理功能
- **Prometheus 监控**：内置 `/metrics` 端点

## 🏗️ 架构概览

```
┌─────────────────────────────────────────────────────────────────┐
│                        HTTP 接入层                              │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐ │
│  │   AiApi         │  │  ChatJsonSink   │  │  ChatSseSink    │ │
│  │   Controller    │  │  ResponsesSink  │  │                 │ │
│  └────────┬────────┘  └─────────────────┘  └─────────────────┘ │
└───────────┼─────────────────────────────────────────────────────┘
            │
┌───────────▼─────────────────────────────────────────────────────┐
│                        适配层                                    │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │  RequestAdapters: HTTP JSON → GenerationRequest             ││
│  └─────────────────────────────────────────────────────────────┘│
└───────────┬─────────────────────────────────────────────────────┘
            │
┌───────────▼─────────────────────────────────────────────────────┐
│                     生成编排层                                   │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐ │
│  │ GenerationService│ │ ExecutionGuard  │  │  ToolCallBridge │ │
│  │   runGuarded()  │  │ (并发门控)       │  │ (工具调用桥接)   │ │
│  └────────┬────────┘  └─────────────────┘  └─────────────────┘ │
└───────────┼─────────────────────────────────────────────────────┘
            │
┌───────────▼─────────────────────────────────────────────────────┐
│                       会话层                                     │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │  chatSession: Hash/ZeroWidth 两种会话追踪模式                ││
│  │  ZeroWidthEncoder: 零宽字符编解码                            ││
│  └─────────────────────────────────────────────────────────────┘│
└───────────┬─────────────────────────────────────────────────────┘
            │
┌───────────▼─────────────────────────────────────────────────────┐
│                     Provider 层                                  │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐ │
│  │   ApiManager    │  │   ApiFactory    │  │  APIinterface   │ │
│  │  (路由/负载均衡) │  │  (Provider注册) │  │  (Provider抽象) │ │
│  └─────────────────┘  └─────────────────┘  └─────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

## 🚀 快速启动

### 使用 Docker Compose

```bash
# 克隆项目后
cd aiapi
docker compose up --build -d
```

### 本地编译

```bash
# 依赖: CMake 3.5+, Drogon, OpenSSL, jsoncpp
mkdir build && cd build
cmake ..
make -j$(nproc)
./aiapi
```

## ⚙️ 配置

### 配置文件

复制 `config.example.json` 为 `config.json` 并修改：

```bash
cp config.example.json src/config.json
```

### 主要配置项

| 配置项 | 说明 | 默认值 |
|--------|------|--------|
| `listeners[].port` | 服务监听端口 | 5555 |
| `db_clients[]` | PostgreSQL 数据库配置 | - |
| `custom_config.session_tracking.mode` | 会话追踪模式 (`hash`/`zerowidth`) | `hash` |
| `custom_config.login_service_urls` | 登录服务地址配置 | - |
| `custom_config.regist_service_urls` | 注册服务地址配置 | - |

### 环境变量

```bash
# 登录服务地址
export LOGIN_SERVICE_URL=http://localhost:5557
```

### 会话追踪模式

在 `config.json` 的 `custom_config.session_tracking.mode` 中配置：

- **`hash`** (默认): 基于消息内容 SHA256 哈希生成会话ID
- **`zerowidth`**: 在响应末尾使用零宽字符嵌入会话ID，对用户不可见

## 📡 API 端点

### Chat Completions API

| 端点 | 方法 | 说明 |
|------|------|------|
| `/chaynsapi/v1/chat/completions` | POST | 聊天补全（支持 stream） |
| `/chaynsapi/v1/models` | GET | 获取可用模型列表 |

### Responses API (OpenAI 兼容)

| 端点 | 方法 | 说明 |
|------|------|------|
| `/chaynsapi/v1/responses` | POST | 创建响应（支持 stream） |
| `/chaynsapi/v1/responses/{id}` | GET | 获取响应详情 |
| `/chaynsapi/v1/responses/{id}` | DELETE | 删除响应 |

### 账户管理 API

| 端点 | 方法 | 说明 |
|------|------|------|
| `/aichat/account/add` | POST | 添加账户 |
| `/aichat/account/info` | GET | 查询账户（内存态） |
| `/aichat/account/dbinfo` | GET | 查询账户（数据库） |
| `/aichat/account/update` | POST | 更新账户 |
| `/aichat/account/delete` | POST | 删除账户 |

### 渠道管理 API

| 端点 | 方法 | 说明 |
|------|------|------|
| `/aichat/channel/add` | POST | 添加渠道 |
| `/aichat/channel/info` | GET | 获取渠道列表 |
| `/aichat/channel/update` | POST | 更新渠道 |
| `/aichat/channel/updatestatus` | POST | 更新渠道状态 |
| `/aichat/channel/delete` | POST | 删除渠道 |

### 监控

| 端点 | 方法 | 说明 |
|------|------|------|
| `/metrics` | GET | Prometheus 指标 |

## 📂 项目结构

```
aiapi/
├── CMakeLists.txt              # 主构建配置
├── config.example.json         # 配置文件模板
├── Dockerfile                  # Docker 构建文件
├── docker-compose.*.yml        # Docker Compose 配置
├── README.md                   # 本文档
├── doc/
│   └── aiapi_callflow_and_api_examples.md  # 详细调用流程文档
└── src/
    ├── main.cc                 # 程序入口
    ├── controllers/            # HTTP 控制器
    │   ├── AiApi.cc/h         # 主 API 控制器
    │   └── sinks/             # 输出格式化器
    │       ├── ChatJsonSink    # Chat API JSON 输出
    │       ├── ChatSseSink     # Chat API SSE 输出
    │       └── ResponsesSseSink # Responses API SSE 输出
    ├── sessionManager/         # 会话管理核心
    │   ├── Session.cc/h        # 会话存储与追踪
    │   ├── GenerationService   # 生成编排服务
    │   ├── GenerationRequest.h # 统一请求结构
    │   ├── RequestAdapters     # 请求适配器
    │   ├── ToolCallBridge      # 工具调用桥接
    │   └── XmlTagToolCallCodec # XML 工具调用编解码
    ├── apiManager/             # Provider 管理
    │   ├── ApiManager          # API 路由与负载均衡
    │   └── ApiFactory          # Provider 工厂
    ├── apipoint/               # Provider 实现
    │   └── chaynsapi/          # chayns API Provider
    ├── accountManager/         # 账户管理
    ├── channelManager/         # 渠道管理
    ├── dbManager/              # 数据库管理
    └── tools/                  # 工具类
        └── ZeroWidthEncoder    # 零宽字符编解码器
```

## 🔧 核心模块说明

### GenerationService

统一的业务编排层，负责：
- 接收 `GenerationRequest` + `IResponseSink`
- 通过 `SessionStore` 获取/更新会话上下文
- 调用 Provider 执行生成
- 将结果转换为 `GenerationEvent` 发送给 Sink
- 统一错误捕获、映射与清理

### RequestAdapters

HTTP → GenerationRequest 转换的唯一实现点：
- `buildGenerationRequestFromChat()`: 解析 Chat Completions API
- `buildGenerationRequestFromResponses()`: 解析 Responses API

### ToolCallBridge

当通道不支持原生 tool calls 时：
- 请求侧：将工具定义注入到 prompt，使用 XML 格式
- 响应侧：解析上游返回的 XML 工具调用块
- 支持 Kilo-Code/RooCode 等严格客户端的兼容

### Session (chatSession)

会话存储与追踪：
- **Hash 模式**: 消息内容 SHA256 生成 key
- **ZeroWidth 模式**: 在输出末尾嵌入零宽字符编码的 sessionId
- **Response API**: 使用 response_id 作为 key

## 🧪 测试

```bash
# 测试模型列表
curl http://localhost:5555/chaynsapi/v1/models

# 测试聊天（非流式）
curl -X POST http://localhost:5555/chaynsapi/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "messages": [{"role": "user", "content": "Hello"}]
  }'

# 测试聊天（流式）
curl -N http://localhost:5555/chaynsapi/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "stream": true,
    "messages": [{"role": "user", "content": "Hello"}]
  }'

# 测试 Responses API
curl -X POST http://localhost:5555/chaynsapi/v1/responses \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "input": "Hello, how are you?"
  }'
```

## 📦 依赖

### 编译依赖

- CMake >= 3.5
- C++17 或更高
- [Drogon](https://github.com/drogonframework/drogon) - C++ Web 框架
- OpenSSL
- jsoncpp

### 运行依赖

- **aiapi-tool**: 账户登录验证服务（独立部署）
- **PostgreSQL**: 数据库（配置在 config.json）

## 📚 详细文档

更详细的调用流程、时序图、接口样例请参考：

- [aiapi 调用关系图 / 时序图 / 接口样例](doc/aiapi_callflow_and_api_examples.md)

## 📄 License

MIT License
