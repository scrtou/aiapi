# aiapi - AI API Gateway Service

AI API 网关服务，提供 OpenAI 兼容的 API 接口。

## 快速启动

### 使用 Docker Compose

```bash
docker compose up --build -d
```

## 配置

### 环境变量

创建 `.env` 文件配置服务：

```bash
# 登录服务地址
LOGIN_SERVICE_URL=http://localhost:5557

# 或使用远程服务器
# LOGIN_SERVICE_URL=https://aiapi-tool.example.com
```

### 部署场景

#### 场景 1: 本地开发（两个服务在同一台机器）

```bash
# 终端 1 - 启动 aiapi-tool
cd ../aiapi_tool
docker compose up -d

# 终端 2 - 启动 aiapi
cd ../aiapi
export LOGIN_SERVICE_URL=http://localhost:5557
docker compose up -d
```

#### 场景 2: 分布式部署（服务在不同服务器）

**服务器 A (aiapi-tool):**
```bash
cd aiapi_tool
docker compose up -d
# 服务运行在 http://server-a:5557
```

**服务器 B (aiapi):**
```bash
cd aiapi
export LOGIN_SERVICE_URL=http://server-a:5557
docker compose up -d
# 服务运行在 http://server-b:5555
```

#### 场景 3: 使用域名

```bash
export LOGIN_SERVICE_URL=https://login.example.com
docker compose up -d
```

## API 端点

- `GET /chaynsapi/v1/models` - 获取可用模型列表
- `POST /chaynsapi/v1/chat/completions` - 聊天补全
- `POST /aichat/account/add` - 添加账户
- `GET curl http://localhost:5555/ /aichat/account/info` - 查询账户信息
- `GET /metrics` - Prometheus 指标

## 测试

```bash
# 测试模型列表
curl http://localhost:5555/chaynsapi/v1/models

# 测试聊天
curl -X POST http://localhost:5555/chaynsapi/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "GPT-4o",
    "messages": [{"role": "user", "content": "Hello"}]
  }'
```

## 依赖服务

- **aiapi-tool**: 账户登录验证服务（独立部署）
- **PostgreSQL/MySQL**: 数据库（配置在 config.json）

