# aiapi

`aiapi` 是一个基于 [Drogon](https://github.com/drogonframework/drogon) 和 C++17 的多 Provider AI API 网关。它向客户端提供 OpenAI Chat Completions、OpenAI Responses 和 Anthropic Messages 风格的 HTTP API，并在服务内部统一完成协议适配、Provider 路由、账号与 Workspace 选择、会话连续性、工具调用桥接、流式输出、错误统计和生命周期管理。

> 文档按当前工作区代码整理，最后更新于 2026-08-17。项目仍在持续重构，理解行为时应以 Controller 路由、`AppWiring.cpp` 的依赖装配和测试为最终依据。

## 目录

- [1. 项目解决什么问题](#1-项目解决什么问题)
- [2. 当前能力与边界](#2-当前能力与边界)
- [3. 快速开始](#3-快速开始)
- [4. 配置文件详解](#4-配置文件详解)
- [5. HTTP API](#5-http-api)
- [6. 整体架构](#6-整体架构)
- [7. 一次生成请求如何执行](#7-一次生成请求如何执行)
- [8. 核心模块](#8-核心模块)
- [9. Provider 实现](#9-provider-实现)
- [10. 会话、续聊与 Responses 索引](#10-会话续聊与-responses-索引)
- [11. 工具调用桥接](#11-工具调用桥接)
- [12. 数据库与持久化](#12-数据库与持久化)
- [13. 启动、并发与优雅停机](#13-启动并发与优雅停机)
- [14. 测试、覆盖率与架构门禁](#14-测试覆盖率与架构门禁)
- [15. 调试与故障排查](#15-调试与故障排查)
- [16. 推荐学习路径](#16-推荐学习路径)
- [17. 扩展项目](#17-扩展项目)

## 1. 项目解决什么问题

不同 AI 上游通常具有不同的认证方式、会话模型、请求格式、工具调用能力和流式协议。`aiapi` 将这些差异收敛到一个网关内：

```text
OpenAI / Claude 客户端
          |
          v
  Drogon HTTP Controller
          |
          v
  Protocol Adapter + Use Case
          |
          v
     GenerationPipeline
      /              \
 ChaynsProvider    RetoolProvider
      \              /
       GenerationEvent
              |
              v
       JSON Sink / SSE Sink
```

项目主要承担以下职责：

- 暴露 OpenAI Chat Completions、OpenAI Responses 和 Anthropic Messages 兼容入口；
- 根据 URL 前缀将请求路由到 `chaynsapi` 或 `retoolapi`；
- 将不同协议请求转换为内部统一的 `GenerationRequest`；
- 管理账号池、账号状态、自动注册、Token 刷新和 Retool Workspace；
- 维护会话连续性、`previous_response_id` 和 response/session 映射；
- 对不原生支持工具调用的上游启用 JSON/XML Tool Bridge；
- 将 Provider 输出转换为统一事件，再编码成 JSON 或 SSE；
- 提供请求限流、管理接口认证、健康检查、Prometheus 指标和错误统计；
- 对后台队列、会话清理线程、账号线程和 Chayns thread reaper 做统一生命周期管理。

## 2. 当前能力与边界

### 2.1 已实现能力

| 类别 | 能力 |
|---|---|
| 客户端协议 | OpenAI Chat Completions、OpenAI Responses、Anthropic Messages |
| 输出方式 | 非流式 JSON、流式 SSE |
| 活跃 Provider | `chaynsapi`、`retoolapi` |
| 模型目录 | 按 Provider 查询模型列表 |
| 工具调用 | 原生工具调用、JSON/XML Bridge、参数规范化、校验、强制工具兜底 |
| 会话 | hash/零宽字符追踪、`previous_response_id`、持久化、过期清理 |
| 账号 | 增删改查、刷新、自动注册、账号池选择、备份 |
| Retool | Workspace 创建、更新、验证、启停、池状态、Workflow/Agent 调用 |
| 运行治理 | 限流、CORS、健康检查、就绪检查、指标、日志、错误事件 |
| 工程质量 | CTest、Drogon 测试、Provider 固件、TSan、Coverage、架构检查 |

### 2.2 重要边界

- `nexosapi` 已退役。历史路由仍存在，但统一返回 HTTP `410 Gone`，用于告诉旧客户端迁移到 `chaynsapi` 或 `retoolapi`。
- AI 生成路由当前没有使用 `AdminAuthFilter`。请求中的 `Authorization` 会进入协议上下文，但网关不会像管理接口那样用 `custom_config.admin_api_key` 校验它。生产环境需要由反向代理或额外过滤器保护公开 AI 路由。
- 所有 `/aichat/*` 管理路由都挂载 `AdminAuthFilter`，但当 `admin_api_key` 为空时会为了兼容旧部署而直接放行。生产配置必须显式设置该值。
- 根路径 `/v1/chat/completions` 和 `/v1/responses` 虽出现在协议 Registry 的内部注册表中，但当前 Drogon Controller 没有暴露这两个 HTTP 路由。客户端应使用带 Provider 前缀的路径。
- `/v1/messages` 是实际暴露的 Claude Messages 路由；没有 Provider 前缀时，Controller 默认选择 `chaynsapi`。
- 当前 `main()` 不解析命令行参数，也不支持 `--config`。配置路径固定为运行目录的 `../config.json`。

## 3. 快速开始

### 3.1 构建依赖

项目固定使用 C++17。CMake 当前直接查找或链接以下依赖：

- CMake 3.5 或更高版本；
- GCC 或 Clang，支持 C++17；
- Drogon；
- OpenSSL；
- JsonCpp；
- PostgreSQL 客户端开发库；
- SQLite/MySQL 对应运行时依赖，取决于实际数据库配置；
- pthread、uuid、zlib 等 Drogon 常用依赖。

即使运行时使用 SQLite，当前 `src/CMakeLists.txt` 仍会执行 `find_package(PostgreSQL REQUIRED)`，因此本地构建仍需安装 PostgreSQL 开发包。

Ubuntu 22.04 可参考：

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake git \
  libssl-dev libjsoncpp-dev libpq-dev \
  libsqlite3-dev default-libmysqlclient-dev \
  uuid-dev zlib1g-dev libspdlog-dev
```

Drogon 可以安装到系统，也可以安装到仓库的 `.deps/drogon-install`。CMake 检测到该目录后会自动将它加入 `CMAKE_PREFIX_PATH`。

### 3.2 准备配置

```bash
cd /home/vps/code/aiapi
cp config.example.json config.json
mkdir -p data logs uploads
```

示例配置默认：

- 监听 `0.0.0.0:55555`；
- 使用名为 `aichatpg` 的 SQLite Drogon DB client；
- 数据库文件为 `./data/aiapi.db`；
- 启用 Prometheus `/metrics` 和 AccessLogger；
- 开启每秒请求限流；
- 管理 API key 默认为空，此时管理接口没有认证保护。

### 3.3 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
```

主程序由 CMake 明确输出到构建目录根部：

```text
build/aiapi
```

测试程序通常位于：

```text
build/src/test/aiapi_test
```

不需要测试时可关闭测试 target：

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DAIAPI_BUILD_TESTS=OFF
cmake --build build-release -j"$(nproc)"
```

### 3.4 启动

`main.cc` 固定加载 `../config.json`，这个路径是相对于进程的当前工作目录，而不是可执行文件位置。因此应从构建目录启动：

```bash
cd /home/vps/code/aiapi/build
./aiapi
```

也可以在仓库根目录执行一个子 Shell：

```bash
(cd build && ./aiapi)
```

不要直接在仓库根目录执行 `./build/aiapi`。此时当前工作目录仍是仓库根目录，程序会错误地尝试读取 `/home/vps/code/config.json`。

启动后验证：

```bash
curl -sS http://127.0.0.1:55555/health | jq
curl -sS http://127.0.0.1:55555/ready | jq
curl -sS http://127.0.0.1:55555/chaynsapi/v1/models | jq
```

`/health` 只表示进程存活；`/ready` 还要求：

- 数据库表可访问；
- `chaynsapi` Provider 已注册；
- 内存账号目录中至少有一个账号。

所以新建空数据库后 `/health` 返回 200、`/ready` 返回 503 是可能的正常状态。

### 3.5 Docker Compose

```bash
mkdir -p data logs cores
cp config.example.json data/config.json
docker network create aiapi_shared 2>/dev/null || true
docker compose up --build
```

Compose 会把以下路径挂载进容器：

| 宿主机 | 容器 | 用途 |
|---|---|---|
| `./data/config.json` | `/usr/aiapi/src/config.json` | 运行配置 |
| `./data` | `/usr/aiapi/src/build/data` | SQLite 等运行数据 |
| `./logs` | `/usr/aiapi/src/build/logs` | 日志 |
| `./cores` | `/usr/aiapi/src/build/cores` | core dump |

容器工作目录是 `/usr/aiapi/src/build`，因此同样通过 `../config.json` 读取 `/usr/aiapi/src/config.json`。Compose 当前映射 `55555:55555`，应与 `data/config.json` 的 listener 端口保持一致。

Docker entrypoint 还支持：

- `CONFIG_JSON`：完全覆盖 `/usr/aiapi/src/config.json`；
- `CUSTOM_CONFIG`：通过 `jq` 将 JSON 对象合并到当前配置顶层。

## 4. 配置文件详解

配置使用 Drogon 标准配置结构，业务配置放在 `custom_config`。完整默认值见 [`config.example.json`](config.example.json)。

### 4.1 Drogon 顶层配置

| 路径 | 说明 | 示例值 |
|---|---|---|
| `listeners[]` | HTTP/HTTPS 监听地址和端口 | `0.0.0.0:55555` |
| `db_clients[]` | Drogon 数据库客户端 | `name=aichatpg`、SQLite |
| `app.number_of_threads` | Drogon IO 线程数 | `4` |
| `app.upload_path` | 上传目录 | `uploads` |
| `app.client_max_body_size` | HTTP body 上限 | `32M` |
| `app.log` | Drogon 自身日志配置 | `logs/aiapi*.log` |
| `plugins` | Drogon 插件 | PromExporter、AccessLogger |

所有 Store 都通过固定名称 `aichatpg` 获取 DB client。启动前的 `ConfigValidator` 会检查该名称，不存在时直接失败，避免服务以半初始化状态启动。

### 4.2 数据库类型

两个位置必须保持一致：

```json
{
  "db_clients": [
    {
      "name": "aichatpg",
      "rdbms": "sqlite3",
      "filename": "./data/aiapi.db"
    }
  ],
  "custom_config": {
    "dbtype": "sqlite3"
  }
}
```

`custom_config.dbtype` 被各持久化组件用来选择 SQLite、MySQL 或 PostgreSQL 方言；`db_clients[].rdbms` 则决定 Drogon 实际创建哪种客户端。只修改其中一个会造成建表或 SQL 方言不匹配。

### 4.3 管理接口认证

```json
{
  "custom_config": {
    "admin_api_key": "replace-with-a-long-random-secret"
  }
}
```

调用管理接口：

```bash
curl -sS \
  -H 'Authorization: Bearer replace-with-a-long-random-secret' \
  http://127.0.0.1:55555/aichat/channel/list | jq
```

`admin_api_key` 为空时过滤器会记录警告并放行请求，仅适合本地开发。

### 4.4 请求大小控制

项目有两层不同的预算，不应混为一个配置：

| 配置 | 作用 |
|---|---|
| `history_replay` | 构造历史上下文时的回放预算，防止历史消息无限增长 |
| `outbound_limits` | 真正发送给每个 Provider 之前的硬性请求大小门禁 |

`history_replay` 主要字段：

- `max_request_bytes`：历史回放后的总请求预算；
- `max_message_bytes`：普通单条消息预算；
- `max_tool_message_bytes`：工具结果消息预算。

`outbound_limits` 支持 `default` 和按 Provider 覆盖：

- `max_request_bytes`：最终上游请求大小；
- `max_message_bytes`：最终单条消息大小；
- `0` 表示不限制。

### 4.5 会话追踪与持久化

```json
{
  "custom_config": {
    "session_tracking": {
      "mode": "zerowidth"
    },
    "session_persistence": {
      "memory_expire_hours": 24,
      "memory_cleanup_interval_hours": 12,
      "db_retention_hours": 336,
      "store_session_payload": true,
      "store_response_body": true
    },
    "response_index": {
      "max_entries": 200000,
      "max_age_hours": 6,
      "cleanup_interval_minutes": 10
    }
  }
}
```

追踪模式：

- `hash`：基于消息内容和上下文计算连续性 key；
- `zerowidth`：在文本中嵌入/解析零宽标记来追踪会话。

持久化选项控制内存 Session TTL、数据库保留期、是否保存 session payload，以及 Responses API 是否保存完整响应 body。

### 4.6 Tool Bridge

关键字段：

| 字段 | 说明 |
|---|---|
| `namespace_enabled` | 是否启用工具命名空间行为 |
| `format` | 默认桥接格式，支持 `json`、`xml` |
| `format_by_channel` | 按渠道覆盖 |
| `format_by_client` | 按客户端覆盖 |
| `format_by_model` | 按模型覆盖 |
| `allow_format_fallback` | 解析失败时是否尝试另一格式 |
| `definition_mode` | 工具定义编码模式 |
| `include_descriptions` | 是否发送工具描述 |
| `max_description_chars` | 单个描述最大字符数 |
| `strict_sentinel` | 是否要求严格哨兵标记 |

格式覆盖顺序是：

```text
全局 format -> channel -> client -> model
```

请求开始后格式会固定。默认 `allow_format_fallback=false`，目的是避免请求使用 JSON、响应却被误按 XML 解析的协议漂移。

### 4.7 限流与 CORS

`custom_config.rate_limit` 控制 OpenAI 风格 Chat/Responses 路由，Claude Messages 使用独立的 `ClaudeRateLimitFilter`，但读取相同的基础限流配置。

```json
{
  "rate_limit": {
    "enabled": true,
    "requests_per_second": 10,
    "burst": 20
  }
}
```

CORS 由 `main.cc` 的 pre-routing/post-handling advice 处理，读取 `custom_config.cors`。这与 `app.cors` 是两个不同配置层；当前自定义 advice 的行为以 `custom_config.cors` 为准。

### 4.8 账号自动化和下游服务

| 配置 | 作用 |
|---|---|
| `account_automation.auto_delete_enabled` | 是否自动删除过期账号 |
| `account_automation.delete_after_days` | 账号自动删除天数 |
| `account_automation.auto_register_enabled` | 是否启用自动注册 |
| `account_background_threads_enabled` | 是否启动账号后台线程 |
| `login_service_urls[]` | 各 Provider 登录服务地址 |
| `regist_service_urls[]` | 各 Provider 注册编排服务地址 |
| `downstream_service_api_keys[]` | 调用下游服务时使用的 Bearer key |

### 4.9 Chayns 浏览器伪装

`custom_config.providers.chaynsapi.browser_impersonation` 用于生成一致的浏览器请求头组合。启用 `per_account_profile` 后，账号会稳定映射到一个 profile。修改时必须保持 `User-Agent`、`sec-ch-ua`、平台和压缩编码相互一致；不要声明 HTTP 客户端无法解码的 `accept_encoding`。

### 4.10 敏感信息

以下内容不应提交到 Git：

- `admin_api_key`；
- 账号密码、Token、Cookie；
- Retool access token、XSRF token、Workflow API key；
- 下游服务 API key；
- 生产数据库、日志、core dump 和请求抓包。

仓库已提供 `config.example.json`，真实配置应保存在被 `.gitignore` 忽略的 `config.json` 或部署系统的 Secret 中。

## 5. HTTP API

### 5.1 AI 生成路由

| 协议 | chaynsapi | retoolapi | 无前缀入口 |
|---|---|---|---|
| Chat Completions | `POST /chaynsapi/v1/chat/completions` | `POST /retoolapi/v1/chat/completions` | 未暴露 |
| Responses Create | `POST /chaynsapi/v1/responses` | `POST /retoolapi/v1/responses` | 未暴露 |
| Responses Get | `GET /chaynsapi/v1/responses/{id}` | `GET /retoolapi/v1/responses/{id}` | 未暴露 |
| Responses Delete | `DELETE /chaynsapi/v1/responses/{id}` | `DELETE /retoolapi/v1/responses/{id}` | 未暴露 |
| Models | `GET /chaynsapi/v1/models` | `GET /retoolapi/v1/models` | 未暴露 |
| Claude Messages | `POST /chaynsapi/v1/messages` | `POST /retoolapi/v1/messages` | `POST /v1/messages`，默认 chaynsapi |

Controller 通过路径前缀推断 Provider：

```text
/retoolapi/... -> retoolapi
其他已注册 AI 路由 -> chaynsapi
```

### 5.2 Chat Completions 示例

非流式：

```bash
curl -sS http://127.0.0.1:55555/chaynsapi/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer client-side-placeholder' \
  -d '{
    "model": "replace-with-model-from-v1-models",
    "messages": [
      {"role": "user", "content": "用一句话解释这个项目"}
    ],
    "stream": false
  }' | jq
```

流式：

```bash
curl -N http://127.0.0.1:55555/chaynsapi/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{
    "model": "replace-with-model-from-v1-models",
    "messages": [
      {"role": "user", "content": "列出三条学习建议"}
    ],
    "stream": true
  }'
```

常用字段：

- `model`：必需，使用 Models API 返回的标识；
- `messages`：Chat 消息数组，不允许为空；
- `stream`：是否返回 SSE；
- `tools`：OpenAI function tool 定义；
- `tool_choice`：`auto`、`required`、`none` 或指定工具；
- `temperature`、`max_tokens` 等参数是否完全生效取决于上游能力。

### 5.3 Responses API 示例

创建：

```bash
curl -sS http://127.0.0.1:55555/chaynsapi/v1/responses \
  -H 'Content-Type: application/json' \
  -d '{
    "model": "replace-with-model-from-v1-models",
    "input": "解释端口与适配器架构",
    "stream": false
  }' | jq
```

续聊：

```bash
curl -sS http://127.0.0.1:55555/chaynsapi/v1/responses \
  -H 'Content-Type: application/json' \
  -d '{
    "model": "replace-with-model-from-v1-models",
    "previous_response_id": "resp_previous_id",
    "input": "结合本项目再举一个例子"
  }' | jq
```

查询与删除：

```bash
curl -sS http://127.0.0.1:55555/chaynsapi/v1/responses/resp_id | jq
curl -sS -X DELETE http://127.0.0.1:55555/chaynsapi/v1/responses/resp_id | jq
```

`GET` 从 `ResponseIndex`/持久化数据读取，不会重新调用 Provider。若 `store_response_body=false`，索引可能只保存 response 到 session 的映射，无法返回完整历史 body。

### 5.4 Claude Messages 示例

```bash
curl -sS http://127.0.0.1:55555/v1/messages \
  -H 'Content-Type: application/json' \
  -H 'anthropic-version: 2023-06-01' \
  -d '{
    "model": "replace-with-model-from-v1-models",
    "max_tokens": 512,
    "messages": [
      {"role": "user", "content": "说明 GenerationEvent 的作用"}
    ],
    "stream": false
  }' | jq
```

Claude Adapter 当前明确校验：

- body 必须是 JSON object；
- `model` 必须存在；
- `max_tokens` 必须是正整数；
- `messages` 必须是非空数组；
- `stream` 若存在必须是布尔值；
- 支持 text、thinking、image、tool_use、tool_result 等内容块，并忽略 `redacted_thinking`。

### 5.5 健康、Prometheus 与管理路由

| 路由 | 说明 | 认证 |
|---|---|---|
| `GET /health` | 进程存活、版本、uptime | 无 |
| `GET /ready` | DB、Provider、账号 readiness | 无 |
| `GET /metrics` | Prometheus Exporter | 无内建 AdminAuth |
| `/aichat/account/*` | 账号管理 | `AdminAuthFilter` |
| `/aichat/channel/*` | 渠道管理 | `AdminAuthFilter` |
| `/aichat/retool/workspace/*` | Retool Workspace 管理 | `AdminAuthFilter` |
| `/aichat/metrics/*` | 请求、错误、状态查询 | `AdminAuthFilter` |
| `/aichat/logs/*` | 日志列表与 tail | `AdminAuthFilter` |

账号路由：

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
```

渠道路由：

```text
GET  /aichat/channel/list
POST /aichat/channel/add
POST /aichat/channel/update
POST /aichat/channel/delete
POST /aichat/channel/update-status
```

Retool Workspace 路由：

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
```

监控路由：

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
```

### 5.6 退役路由

以下 Nexos 路由保留 tombstone，并返回 HTTP 410：

```text
POST   /nexosapi/v1/chat/completions
GET    /nexosapi/v1/models
GET    /nexosapi/v1/account/quota
POST   /nexosapi/v1/responses
GET    /nexosapi/v1/responses/{id}
DELETE /nexosapi/v1/responses/{id}
```

## 6. 整体架构

项目采用端口与适配器思想，并用 CMake target 和架构脚本约束分层。

```text
src/
├── domain/          纯领域模型、策略、端口接口
├── application/     用例和业务流程编排
├── infrastructure/  DB、HTTP、Provider、后台执行器等实现
├── transport/       Drogon Controller、Filter、Codec、Response Sink
├── runtime/         Composition Root、启动与关闭生命周期
├── platform/        Result/Error、取消、deadline、日志等通用能力
└── test/            单元、集成、固件和生命周期测试
```

### 6.1 依赖方向

概念上的依赖方向：

```text
platform
   ^
domain
   ^
application
   ^             ^
transport    infrastructure
       \       /
         runtime
```

CMake target 对应：

| Target | 主要内容 | 关键依赖 |
|---|---|---|
| `aiapi_platform` | 通用基础能力 | 无业务层依赖 |
| `aiapi_domain` | 领域接口 target | `aiapi_platform` |
| `aiapi_application` | 用例与生成流程 | domain、platform |
| `aiapi_infrastructure` | DB、Provider、HTTP 实现 | domain、platform、Drogon、OpenSSL、PostgreSQL |
| `aiapi_transport` | Controller、Filter、Sink | application、domain、platform、Drogon |
| `aiapi_runtime` | 依赖装配 | application、infrastructure、transport |
| `aiapi` | 程序入口 | transport、runtime |

生产 `.cpp/.cc` 通过 `aiapi_register_production_sources()` 保证只有一个正式 target 拥有；同一源文件被重复加入多个生产 target 时，CMake 配置阶段会失败。

### 6.2 各目录阅读重点

| 路径 | 先看什么 | 学习目标 |
|---|---|---|
| `src/main.cc` | 配置、CORS、Drogon advice、shutdown | 理解进程入口 |
| `src/runtime/AppWiring.cpp` | `registerApplicationSteps()` | 理解所有依赖如何组装 |
| `src/domain/model` | ProviderRequest/Response、SessionData、Capabilities | 理解稳定领域数据 |
| `src/domain/port` | `IChatProvider`、`IAccountStore` 等 | 理解依赖反转 |
| `src/application/generation/contracts` | Request、Event、Sink | 理解协议无关契约 |
| `src/application/generation/core` | Pipeline、Session、UseCase | 理解生成主链路 |
| `src/application/generation/protocol` | OpenAI/Claude Adapter 和 Sink | 理解边界转换 |
| `src/application/generation/tooling` | Tool Bridge | 理解工具调用兼容层 |
| `src/infrastructure/provider` | ProviderBase、Chayns、Retool | 理解上游调用 |
| `src/infrastructure/persistence` | 各 DbManager | 理解数据库边界 |
| `src/transport/controllers` | 路由与 HTTP 状态映射 | 理解 HTTP 接入 |
| `src/test` | 与模块同名的测试 | 用行为反证理解是否正确 |

## 7. 一次生成请求如何执行

以 `POST /chaynsapi/v1/chat/completions` 为例：

```text
1. AiApiController 接收 HTTP 请求
2. RateLimitFilter 执行令牌桶限流
3. Controller 解析 JSON、识别 stream、根据路径推断 Provider
4. AiApiUseCase 通过 ProtocolRegistry 分派协议和 operation
5. OpenAiRequestAdapter 将请求转为 GenerationRequest
6. BackgroundTaskQueue 接受异步生成任务
7. GenerationPipeline 解析连续性、加载 Session、获取执行门控
8. ProviderRegistry 找到 ChaynsProvider
9. ProviderBase 建立统一 deadline/cancellation/telemetry 边界
10. ChaynsProvider 选择账号、创建或复用 thread、发送消息并轮询
11. GenerationResponsePipeline 清洗文本、解析工具调用并发出 GenerationEvent
12. OpenAiChatJsonSink 或 OpenAiChatSseSink 编码客户端响应
13. Pipeline 提交 Session，并在 Responses 场景更新 ResponseIndex
14. 错误和请求聚合写入 metrics/error stats
15. 执行门控释放，异步任务结束
```

流式请求不会让后台线程直接操作 Drogon 的 response stream。`IoLoopResponseStream` 将发送和关闭操作串行切回 Drogon IO loop，降低跨线程操作 HTTP 对象的风险。

## 8. 核心模块

### 8.1 ProtocolRegistry

位置：`src/application/generation/protocol/common/`

Registry 将 `HTTP method + path` 映射为：

- 协议模块 ID；
- operation 名称；
- Request Adapter；
- Response Sink Factory；
- 协议能力映射。

默认注册两个协议模块：

- `openai-compatible`；
- `anthropic-messages`。

这样 Controller 不需要为每个协议复制完整生成流程，核心 Pipeline 也不解析 OpenAI/Claude 原始 JSON。

### 8.2 AiApiUseCase

位置：`src/application/generation/core/AiApiUseCase.*`

这是 transport 调用的应用层入口，主要负责：

- 协议分派；
- 模型能力解析；
- Sink 创建；
- 后台任务提交；
- shutdown 时拒绝新任务；
- 将内部 Result/Error 转换成 Controller 可消费的结果。

### 8.3 GenerationService 与 GenerationPipeline

`GenerationService` 是稳定的薄 facade，实际编排在 `GenerationPipeline`：

- 物化统一请求；
- 决定会话连续性；
- 加载/创建 Session；
- 获取同一 Session 的执行门控；
- 调用 Provider；
- 执行响应处理；
- 提交 Session 和 response 映射；
- 统一处理取消、超时和失败。

### 8.4 GenerationResponsePipeline

负责 Provider 返回之后的处理：

- 清理不应暴露给客户端的文本；
- 解析原生或 Bridge 工具调用；
- 工具参数规范化；
- 工具调用校验；
- 必要时生成 forced tool call；
- 发射 TextDelta、ToolCall、Completed、Error 等统一事件。

### 8.5 Generation contracts

位置：`src/application/generation/contracts/`

| 契约 | 作用 |
|---|---|
| `GenerationRequest` | 协议无关的统一请求 |
| `GenerationSession` | 一次执行所需的聚合状态 |
| `GenerationEvent` | 协议无关的输出事件 |
| `IResponseSink` | JSON/SSE 输出边界 |

核心消息只使用 canonical `Message::blocks`。协议原始工具树只能放在 `protocolExtensions`，Pipeline 不应读取或解释特定厂商 JSON 字段。

### 8.6 账号与渠道

`application/account` 负责账号选择、注册、Token 工作流、健康检查和后台线程；`application/channel` 负责渠道增删改查及状态。

账号选择不仅是简单轮询，还会考虑：

- Provider/API 名称；
- active/waiting/retired 等状态；
- Token 和账号可用性；
- 使用次数和池顺序；
- Workspace 绑定；
- 本轮已经尝试过的账号；
- 渠道是否启用及并发限制。

### 8.7 Metrics 与 ErrorStats

错误按事件和小时聚合保存：

- `error_event`：单次错误详情；
- `error_agg_hour`：错误小时聚合；
- `request_agg_hour`：请求小时聚合。

`MetricsUseCase` 将查询接口与具体 DB manager 隔离，Controller 只通过 use case 获取数据。

## 9. Provider 实现

### 9.1 ProviderRegistry 和 ProviderBase

`ProviderRegistry` 保存 `IChatProvider`、模型目录和 thread context。生产 Provider 在 `AppWiring.cpp` 中构造并注册，之后由生成流程按 Provider ID 查询。

`ProviderBase` 使用薄模板方法边界统一处理：

- 请求上下文；
- deadline；
- cancellation；
- telemetry；
- 公共错误和调用约束。

Provider 特有协议保留在各自目录中，不把 Chayns thread 语义强行复用到 Retool。

### 9.2 ChaynsProvider

位置：`src/infrastructure/provider/chayns/`

主要组成：

| 模块 | 责任 |
|---|---|
| `ChaynsProvider` / `chaynsapi` | Provider 主调用流程 |
| `ChaynsModelCatalog` | 模型目录 |
| `ChaynsHttpTransport` | HTTP 传输 |
| `ChaynsProtocolClient` | 上游 wire 协议 |
| `ChaynsThreadContext` | thread 生命周期上下文 |
| `ChaynsPollingLoop` | 可取消轮询 |
| `ChaynsMessageCorrelation` | 消息关联与去重 |
| `ChaynsProviderPolicy` | Provider 策略 |
| `chaynsThreadReaper` | 过期 thread 回收 |

典型流程：选择账号 -> 创建/恢复 thread -> 可选图片上传 -> 创建消息 -> 轮询结果 -> 关联 assistant 消息 -> 更新 thread ledger。

### 9.3 RetoolProvider

位置：`src/infrastructure/provider/retool/`

Retool 支持两条上游路径：

- Workflow：由 `RetoolWorkflowClient` 执行；
- Agent：由 `RetoolAgentClient` 创建/复用 thread 并执行。

`RetoolWorkspaceContext` 和 Workspace Store 负责选择、固定和释放 Workspace，同时维护健康、启停和 in-use 状态。OpenAI/Anthropic 资源 UUID、Workflow/Agent ID 等均属于 Workspace 数据，而不是写死在协议 Adapter 中。

## 10. 会话、续聊与 Responses 索引

### 10.1 ContinuityResolver

位置：`src/application/generation/continuity/ContinuityResolver.*`

连续性决策按请求信息选择：

- 新建会话；
- 通过 `previous_response_id` 恢复；
- 通过零宽标记恢复；
- 通过消息 hash 恢复。

续聊策略集中在 Resolver，Controller 不负责拼接历史。

### 10.2 Session

`chatSession` 管理内存会话、上下文和过期清理。`SessionDbManager` 实现 `ISessionPersistence`，负责将会话快照写入 `chat_session_state`。

同一 Session 的并发执行由 `SessionExecutionGate` 控制，避免两个请求同时修改同一上下文。冲突会映射为 HTTP 409，而不是让 Provider 状态无序覆盖。

### 10.3 ResponseIndex

`ResponseIndex` 维护：

```text
response_id -> session_id -> 可选的完整 response body
```

它用于：

- `previous_response_id` 查找会话；
- `GET /responses/{id}`；
- `DELETE /responses/{id}`；
- 定期清理过旧或超量索引。

### 10.4 HistoryReplayBudget

历史回放以完整 turn 为保留单位，并分别限制普通消息和工具结果。目标是避免简单按字节截断导致 assistant tool call 与 user tool result 失配。

## 11. 工具调用桥接

部分上游不支持原生 function calling。Tool Bridge 会把工具定义编码到上游可理解的提示中，再把模型文本解析为统一 Tool Call。

```text
客户端 tools
    |
ToolDefinitionEncoder
    |
JSON/XML bridge prompt
    |
Provider text response
    |
BridgeProtocolCodec / XmlTagToolCallCodec
    |
ToolCallNormalizer
    |
ToolCallValidator
    |
GenerationEvent::ToolCall*
```

主要类：

| 类 | 责任 |
|---|---|
| `ToolDefinitionEncoder` | 编码工具定义和描述 |
| `BridgeProtocolCodec` | 固定请求级 JSON/XML 格式并解析响应 |
| `ToolCallBridge` | Bridge 主入口 |
| `XmlTagToolCallCodec` | XML 工具标签编解码 |
| `ToolCallNormalizer` | 将字符串化 JSON、别名等规范为标准参数 |
| `ToolCallValidator` | 校验工具名、参数和选择策略 |
| `StrictClientRules` | Codex 等严格客户端规则 |
| `ForcedToolCallGenerator` | `tool_choice=required` 等场景的兜底 |
| `ToolResultLedger` | 追踪工具结果，避免重复或错配 |

调试工具问题时应按“请求编码 -> 上游原文 -> 解析 -> 规范化 -> 校验 -> Sink 输出”的顺序逐段检查，而不是只看最终 400/502。

## 12. 数据库与持久化

### 12.1 Store 边界

domain 只定义端口，例如：

- `IAccountStore`；
- `IChannelStore`；
- `IRetoolWorkspaceStore`；
- `ISessionPersistence`；
- `IChaynsThreadLedger`；
- `IKeyValueConfigStore`；
- `IAccountBackupStore`。

SQL、JsonCpp 和 Drogon DbClient 都位于 infrastructure。这样 application 可以在测试中替换 stub，而不依赖真实数据库。

### 12.2 主要表

| 表 | 用途 |
|---|---|
| `account` | Provider 账号、Token、状态、使用次数、Workspace 绑定 |
| `account_backup` | 账号变更前备份 |
| `channel` | 渠道 URL、key、状态、并发、超时、工具能力 |
| `app_config` | 动态 key-value 配置 |
| `retool_workspace` | Retool 凭据、资源、Workflow/Agent、状态与用量 |
| `chaynsa_thread` | Chayns thread ledger、消息关联和回收状态 |
| `chat_session_state` | 会话快照 |
| `response_index` | response/session 映射与可选响应 body |
| `error_event` | 错误事件明细 |
| `error_agg_hour` | 错误小时聚合 |
| `request_agg_hour` | 请求小时聚合 |

各 DbManager 会在初始化时执行 `CREATE TABLE IF NOT EXISTS` 和必要的兼容性列迁移。涉及生产升级时，仍应先备份数据库并检查 `tools/migrations/` 中的迁移脚本，尤其是 Provider 退役相关脚本。

### 12.3 SQLite 路径规则

示例中的 `filename=./data/aiapi.db` 同样相对于进程当前工作目录。按推荐方式从 `build/` 启动时，实际文件是：

```text
/home/vps/code/aiapi/build/data/aiapi.db
```

Docker 中对应 `/usr/aiapi/src/build/data/aiapi.db`，并映射到宿主机 `./data/aiapi.db`。

## 13. 启动、并发与优雅停机

### 13.1 启动阶段

`main()` 先加载和校验 JSON，但 DB client 只有进入 `drogon::app().run()` 后才创建。因此真正的应用装配注册在 BeginningAdvice 中：

```text
loadConfigFile
  -> ConfigValidator
  -> registerApplicationSteps
  -> drogon::app().run
  -> BeginningAdvice
  -> AppContext::build
  -> HTTP listener ready
```

`registerApplicationSteps()` 当前依次装配：

1. BackgroundTaskQueue；
2. session tracking mode；
3. runtime persistence stores；
4. account/channel/workspace store 注入；
5. ProviderRegistry；
6. session services；
7. AI API use case；
8. error stats；
9. session persistence；
10. Chayns thread ledger；
11. session tuning；
12. session cleaner；
13. response index cleanup。

任何必需步骤失败都会返回 `StartupResult`，`AppContext` 回滚已启动 owner，然后退出，不会继续暴露一个半初始化服务。

### 13.2 BackgroundTaskQueue

生成请求和部分管理操作通过有界后台队列执行。队列提供：

- worker 线程；
- enqueue 结果；
- 队列关闭后拒绝新任务；
- shutdown drain；
- deadline 控制；
- worker 异常隔离。

这也是背压边界：队列无法接受任务时，应返回可观测错误，而不是无限创建线程。

### 13.3 Cancellation 与 Deadline

项目使用 `platform/Cancellation.h` 和 `platform/Deadline.h` 将取消和截止时间贯穿 application、Provider、HTTP 和轮询边界。客户端断开、超时或停机时，阻塞操作应尽快观察取消信号。

### 13.4 停机顺序

进程收到终止信号后，Drogon `run()` 返回，`AppContext` 使用同一个绝对 deadline 逆序停止 owner。当前总宽限期是 25 秒，用于适配容器常见的 30 秒终止窗口。

关键约束是：

```text
先停止任务生产者和清理线程
再 drain BackgroundTaskQueue
最后释放它们依赖的对象
```

逆序 owner 注册避免在 `main.cc` 再维护一份独立的停机顺序。已知限制是某些同步上游 HTTP 调用无法被 C++17 `thread::join` 强制中断，相关开放问题见 [`docs/D11-shutdown-open-questions.md`](docs/D11-shutdown-open-questions.md)。

## 14. 测试、覆盖率与架构门禁

### 14.1 普通测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

也可以直接运行 Drogon 测试二进制：

```bash
./build/src/test/aiapi_test
```

按用例过滤时使用测试程序支持的参数，例如：

```bash
./build/src/test/aiapi_test -r TestCaseName
```

测试覆盖的重点包括：

- OpenAI/Claude 协议 Adapter 和 Sink；
- Tool Bridge、XML/JSON codec、参数规范化和校验；
- continuity、response index、history replay；
- Chayns/Retool Provider 固件；
- 账号状态机、池排序、刷新和自动注册；
- Controller use case 注入；
- DB store 端口；
- shutdown、取消、后台队列、IO loop stream；
- Provider 退役和配置校验。

### 14.2 Fixture

`src/test/fixtures/` 保存脱敏的 Chayns/Retool JSON 响应。Provider 测试应优先通过 transport seam 和 fixture 验证，不应在普通单元测试中访问真实上游。

### 14.3 ThreadSanitizer

```bash
./tools/run-tsan.sh
```

脚本会：

1. 配置并构建 `build-tsan`；
2. 运行全量单测；
3. 重复执行停机专项；
4. 运行 idle/http/polling/backlog/disconnect 信号夹具；
5. 检查数据竞争和停机顺序标记。

可通过环境变量调整重复次数和超时，具体见脚本开头。

### 14.4 Coverage

```bash
cmake -S . -B build-coverage \
  -DCMAKE_BUILD_TYPE=Debug \
  -DAIAPI_ENABLE_COVERAGE=ON
cmake --build build-coverage -j"$(nproc)"
ctest --test-dir build-coverage --output-on-failure
python3 tools/coverage/generate_report.py --build-dir build-coverage
```

Coverage 工具只汇总正式生产 library target 和 `aiapi_test` 产生的 gcda，避免把未执行的主程序 target 混入生产覆盖率分母。

### 14.5 架构检查

架构规则位于 `tools/arch/`，覆盖：

- include 根路径；
- target 分层；
- source 唯一所有权；
- Controller/use case 边界；
- Provider foundation 和 Registry；
- HTTP IO 边界；
- 生命周期、shutdown deadline 和 enqueue 结果；
- 依赖环和物理目录布局；
- 退役 Provider 残留。

先阅读 [`tools/arch/README.md`](tools/arch/README.md)，再按其中命令运行单项检查。全局审计入口是：

```bash
python3 tools/architecture_audit.py
```

CI 中 `.github/workflows/arch-cycles.yml` 还会检查依赖环基线。

## 15. 调试与故障排查

### 15.1 日志位置

项目存在两套日志来源：

- Drogon 日志：由 `app.log.log_path` 和 `logfile_base_name` 控制；
- application 日志：`main.cc` 读取 `custom_config.log`，未配置时默认写入 `logs/aiapi.application.log`。

按推荐方式从 `build/` 启动时，默认日志目录是 `build/logs/`。

### 15.2 推荐排查顺序

1. `GET /health`：确认进程存在；
2. `GET /ready`：查看 DB、Provider、账号哪一项失败；
3. `GET /chaynsapi/v1/models` 或 Retool 对应路由：确认模型目录；
4. `/aichat/channel/list`：确认渠道启用、超时和并发设置；
5. `/aichat/account/info`：确认账号数量和状态；
6. `/aichat/retool/workspace/pool-status`：确认 Retool Workspace；
7. `/aichat/metrics/status/summary`：查看整体状态；
8. `/aichat/metrics/errors/events`：定位具体错误事件；
9. `/aichat/logs/tail`：结合 request ID 和时间查看日志。

### 15.3 常见问题

#### 启动时报找不到 config.json

确认进程当前工作目录是 `build/`：

```bash
pwd
ls -l ../config.json
```

#### 启动时报缺少 aichatpg

`db_clients` 中必须存在：

```json
{"name": "aichatpg"}
```

名称区分大小写，不要随意改为 `default`。

#### `/ready` 返回 503

查看响应中的 `checks.database`、`checks.provider`、`checks.account`。空账号库会导致 account readiness 失败。

#### 请求返回 429

先区分是网关 `RateLimitFilter`，还是上游 Provider 限流。检查 `Retry-After`、错误 type、错误事件中的 domain/provider 和 `custom_config.rate_limit`。

#### 请求返回 409

通常表示同一 Session 已有执行正在进行。检查客户端是否重复提交同一会话、`previous_response_id` 或 session header。

#### 请求返回 502/504

502 通常是 Provider/上游错误，504 是 deadline 超时。检查账号、Workspace、上游 URL、轮询阶段和取消日志。

#### 工具调用无法解析

检查实际生效的 bridge format、客户端/model/channel 覆盖、上游原始输出、strict sentinel、tool name 和 JSON Schema。

#### 上下文过长

同时检查 `history_replay` 和对应 Provider 的 `outbound_limits`。前者决定历史如何裁剪，后者决定最终请求能否发送。

#### 流式响应被代理缓存

服务会设置 `Content-Type: text/event-stream`、`Cache-Control: no-cache` 和 `X-Accel-Buffering: no`。反向代理仍需关闭 SSE buffering，并配置足够长的 read timeout。

## 16. 推荐学习路径

建议不要从最大的 Provider `.cpp` 开始。按以下顺序更容易建立完整心智模型：

### 第一阶段：看外部行为

1. 阅读 `src/transport/controllers/AiApiController.h`，列出真实路由；
2. 阅读 `AiApiController.cc`，理解 JSON、SSE 和错误如何进入 use case；
3. 阅读 `test_openai_request_adapter.cpp`、`test_claude_protocol.cpp`、`test_sinks.cpp`；
4. 本地调用 `/health`、`/ready` 和 `/v1/models`。

### 第二阶段：看统一生成模型

1. `contracts/GenerationRequest.h`；
2. `contracts/GenerationEvent.h`；
3. `contracts/IResponseSink.h`；
4. OpenAI/Claude Request Adapter；
5. OpenAI/Claude JSON/SSE Sink。

完成后应能回答：为什么核心生成逻辑不需要知道客户端是 OpenAI 还是 Claude？

### 第三阶段：看主流程

1. `AiApiUseCase.cpp`；
2. `GenerationService.cpp`；
3. `GenerationPipeline.cpp`；
4. `GenerationResponsePipeline.cpp`；
5. `SessionExecutionGate.h`；
6. `test_ai_api_use_case.cpp` 和 `test_generation_service_emit.cpp`。

完成后应能画出“Adapter -> Pipeline -> Provider -> Event -> Sink”的调用图。

### 第四阶段：看会话和工具

1. `ContinuityResolver.cpp`；
2. `ResponseIndex.cpp`；
3. `Session.cpp` / `SessionCodec.cpp`；
4. `BridgeProtocolCodec.cpp`；
5. `ToolCallNormalizer.cpp`；
6. `ToolCallValidator.cpp`；
7. 对应的 continuity/tooling 测试。

### 第五阶段：看 Provider

先读公共边界：

1. `domain/port/IChatProvider.h`；
2. `ProviderBase.h/.cpp`；
3. `ProviderRegistry.h/.cpp`；
4. `ProductionProviderFactory.h`。

再分别追踪：

- Chayns：model catalog -> account selector -> protocol client -> polling -> correlation -> ledger；
- Retool：workspace context -> workflow/agent client -> HTTP protocol -> usage release。

### 第六阶段：看运行时和工程约束

1. `main.cc`；
2. `AppContext.h/.cpp`；
3. `AppWiring.cpp`；
4. `BackgroundTaskQueue.h`；
5. shutdown/cancellation 测试；
6. `doc/adr/decisions/` 和 `tools/arch/`。

此阶段重点不是业务功能，而是理解为什么对象由 Composition Root 唯一构造、为什么 owner 逆序关闭、为什么 HTTP/DB 只能出现在边缘。

## 17. 扩展项目

### 17.1 新增客户端协议

推荐步骤：

1. 在 `application/generation/protocol/<protocol>/` 新增 Request Adapter；
2. 实现 JSON/SSE Sink 和 Error Formatter；
3. 实现 `IProtocolModule`，声明 operation 和 capability；
4. 在 `makeDefaultProtocolRegistry()` 注册 module 和 route；
5. 在 Controller 暴露实际 Drogon 路由；
6. 增加 Adapter、Sink、Registry 和端到端测试；
7. 不要让 `GenerationPipeline` 解析协议原始 JSON。

仅在 Registry 中注册 route 不等于已经对 HTTP 暴露；Controller 的 `ADD_METHOD_TO` 也必须存在。

### 17.2 新增 Provider

推荐步骤：

1. 实现 `IChatProvider`，通常复用薄 `ProviderBase`；
2. 定义 Provider 自己的 HTTP transport、wire codec、策略和模型目录；
3. 明确账号或 Workspace 选择边界；
4. 在 `AppWiring.cpp` 构造并注册；
5. 配置独立 `outbound_limits.<provider>`；
6. 增加脱敏 fixture 和 provider test；
7. 增加 readiness、metrics 和错误映射；
8. 运行架构门禁，确保 infrastructure 没有反向渗入 application/domain。

### 17.3 新增管理功能

遵循以下链路：

```text
Controller -> application use case -> domain port -> infrastructure store/client
```

Controller 只负责 HTTP 解析、认证过滤器、状态码和 JSON codec。业务规则应进入 use case，SQL 和上游 HTTP 应进入 infrastructure，并通过端口注入以便测试。

## 进一步阅读

- [`doc/adr/decisions/README.md`](doc/adr/decisions/README.md)：当前架构决策索引；
- [`doc/adr/design/module-catalog.md`](doc/adr/design/module-catalog.md)：模块和职责目录；
- [`doc/adr/design/flow-contracts.md`](doc/adr/design/flow-contracts.md)：业务流程调用契约；
- [`doc/multi-protocol-generation/README.md`](doc/multi-protocol-generation/README.md)：多协议生成设计；
- [`doc/session/README.md`](doc/session/README.md)：会话连续性文档；
- [`src/application/generation/README.md`](src/application/generation/README.md)：生成模块入口；
- [`src/application/generation/tooling/README.md`](src/application/generation/tooling/README.md)：Tool Bridge；
- [`tools/migrations/README.md`](tools/migrations/README.md)：数据库迁移；
- [`docs/D11-shutdown-open-questions.md`](docs/D11-shutdown-open-questions.md)：停机已知问题。

## 开发约束摘要

- domain 保持纯模型和端口，不依赖 Drogon、JsonCpp 或 SQL；
- application 编排用例，不直接执行 HTTP/DB；
- transport 不堆业务逻辑；
- infrastructure 实现端口，不向核心层暴露 wire JSON；
- 新线程、队列、定时器和长生命周期对象必须登记 owner 和 shutdown 行为；
- 外部阻塞操作必须考虑 deadline 与 cancellation；
- 修改 API、协议、Provider 或生命周期时同步增加测试；
- 不提交 Token、Cookie、密码、生产数据库、日志和抓包。

## License

仓库当前未声明明确的开源许可证。使用、分发或二次开发前，请向项目维护者确认授权范围。
