# 服务状态监控（渠道/模型）— 详细设计文档

> 目标：新增一个“服务状态”监控页面（参考你提供的截图样式），用于监控 **渠道（Channel）** 与 **模型（Model）** 的服务可用性、成功率、延迟与近一段时间的趋势。
>
> 覆盖范围：
> 1) `aiapi/`（后端 C++ Drogon 项目）：接口设计 + 数据库设计 + 采集/聚合逻辑
> 2) `aiapi_web/`（前端 React/Vite/TS）：页面信息架构 + 组件设计 + API/类型设计

---

## 1. 背景与现状

当前项目已具备：
- 渠道管理（`channelManager`, `channelDbManager`）
- 模型列表接口（OpenAI 兼容 `GET /chaynsapi/v1/models`）
- 错误监控与请求/错误统计（前端 `ErrorMonitor` 组件已接入 `/aichat/metrics/*`）

我们希望新增一个“服务状态”页，提供类似截图的：
- 顶部：最近更新时间 + 总体 API 状态（正常/异常/未知）
- 列表：按渠道/模型展示
  - 可用率（%）
  - 请求数、成功数
  - 近 N 个时间桶的小柱状图（成功/失败或仅成功率）
  - 右侧状态徽章：正常/异常/未知

---

## 2. 目标与非目标

### 2.1 目标（Must）
1. **渠道状态监控**：按渠道统计请求量/成功量/可用率，并给出状态。
2. **模型状态监控**：按模型统计请求量/成功量/可用率，并给出状态（可按 provider / channel 维度细分）。
3. **趋势可视化**：提供近一段时间（例如 1h/6h/24h/7d）的小时间序列。
4. **可扩展**：后续可加入主动探测（probe）、更多维度（client_type、api_kind、stream）。

### 2.2 非目标（暂不做 / Nice-to-have）
- 不做复杂告警系统（短信/邮件/钉钉），但设计保留扩展点。
- 不做 Prometheus/Grafana 集成（已有 `/metrics` 可后续对接）。
- 不做实时 WebSocket 推送，先用轮询刷新。

---

## 3. 总体方案概览

我们提供两种数据来源，并在接口层统一输出：

1) **被动观测（推荐优先）**：从网关自身的请求日志/事件中统计“真实流量”成功率与延迟。
- 优点：反映真实用户体验。
- 缺点：无流量时是“未知”。

2) **主动探测（可选增强）**：定时对渠道/模型发起轻量探测请求（例如最小 prompt 或 `/models`），记录探测成功/失败与 RTT。
- 优点：无流量也能判断“活着/死了”。
- 缺点：会产生额外调用与成本。

本次设计文档：
- **接口/DB 以“被动观测 + 可选主动探测”双轨设计**。
- 前端默认展示被动观测指标；如果启用 probe，可在 UI 增加“探测状态”。

---

## 4. 后端设计（aiapi/）

### 4.1 新增模块与文件建议

建议新增：
- `src/controllers/ServiceStatus.cc/.h`（Drogon Controller）
- `src/dbManager/monitor/`（新 DB manager）
  - `serviceStatusDbManager.cpp/.h`
- `src/monitor/`（可选：聚合/探测逻辑）
  - `StatusAggregator.cpp/.h`
  - `StatusProbeScheduler.cpp/.h`（可选）

与现有结构保持一致：controller 负责路由与参数校验；dbManager 负责 SQL；业务逻辑独立。

### 4.2 数据库设计

> 假设使用 PostgreSQL（项目已有 `db_clients` 支持）。

#### 4.2.1 表：被动观测明细（可选，若已有请求日志表可复用）
如果系统已经有“请求事件/错误事件”表，优先复用；否则新增轻量明细表用于后续聚合。

**表：`aichat_request_events`（建议）**
- `id bigserial primary key`
- `ts timestamptz not null`
- `request_id text`
- `channel_id int`（可空；取决于请求是否命中渠道）
- `channel_name text`
- `provider text`
- `model text`
- `api_kind text`（chat/responses/models…）
- `client_type text`
- `stream boolean`
- `http_status int`
- `latency_ms int`
- `success boolean not null`
- `error_type text`（失败时）
- `error_message text`（失败时，注意长度）

索引：
- `(ts)`
- `(ts, channel_id)`
- `(ts, model)`
- `(request_id)`

> 若已有类似记录表，可只新增聚合表。

#### 4.2.2 表：状态聚合桶（核心）
用于快速查询趋势与列表。

**表：`aichat_status_buckets`**
- `bucket_start timestamptz not null`（UTC，桶起始）
- `bucket_minutes int not null`（例如 1/5/15/60/360/1440）
- `entity_kind text not null`（'API' | 'CHANNEL' | 'MODEL'）
- `entity_key text not null`
  - API: 'api'
  - CHANNEL: `channel:{id}` 或 `channel:{name}`（建议用 id）
  - MODEL: `model:{provider}:{model}`（必要时加入 channel）
- `entity_name text not null`
- `requests int not null default 0`
- `success int not null default 0`
- `fail int not null default 0`
- `avg_latency_ms int`（可空）
- `p95_latency_ms int`（可选扩展）
- `last_http_status int`（可选扩展）
- `meta jsonb`（可选：维度扩展）

主键/唯一：
- unique `(bucket_start, bucket_minutes, entity_kind, entity_key)`

索引：
- `(entity_kind, entity_key, bucket_minutes, bucket_start desc)`

#### 4.2.3 表：主动探测结果（可选增强）
**表：`aichat_probe_events`**
- `id bigserial primary key`
- `ts timestamptz not null`
- `entity_kind text not null`（CHANNEL|MODEL|API）
- `entity_key text not null`
- `ok boolean not null`
- `latency_ms int`
- `http_status int`
- `error text`
- `detail jsonb`

索引：
- `(entity_kind, entity_key, ts desc)`

> 是否启用 probe：通过配置 `custom_config.service_status.probe_enabled` 控制。

### 4.3 状态判定规则（后端输出时计算）

状态枚举：
- `OK`（正常）
- `DEGRADED`（波动/部分失败）
- `DOWN`（严重失败）
- `UNKNOWN`（无数据）

默认阈值（可配置）：
- `ok_threshold = 0.99`
- `degraded_threshold = 0.95`
- `min_requests_for_confidence = 20`（低于此值可降权或显示 UNKNOWN/DEGRADED）
- `stale_after_seconds = 180`（例如 3 分钟内无新桶则认为数据过期）

判定：
- 有请求：`availability = success/requests`
  - >= ok_threshold → OK
  - >= degraded_threshold → DEGRADED
  - else → DOWN
- 无请求：
  - 若启用 probe 且最近 probe ok → OK
  - 若启用 probe 且最近 probe fail → DOWN
  - 否则 → UNKNOWN

### 4.4 聚合策略

#### 4.4.1 方案 A：查询时聚合（简单但可能慢）
每次 API 调用直接对 `aichat_request_events` 按时间桶聚合。
- 优点：无需后台任务
- 缺点：数据量大时慢

#### 4.4.2 方案 B：后台定时聚合（推荐）
通过 drogon 定时器每分钟/每5分钟写入 `aichat_status_buckets`。
- 例如每 60 秒聚合上一分钟数据，写入 bucket_minutes=1
- 同时可滚动生成 5m/15m/1h 桶（或前端只用 1m，后端返回时可再聚合）

推荐实现：
- 先做 `bucket_minutes=1`（粒度最高）
- API 支持 `interval=1m|5m|15m|1h|6h|1d`，返回时由 SQL `date_trunc` + `GROUP BY` 合并

### 4.5 后端接口设计

> 命名风格建议与现有 `/aichat/metrics/...` 一致：新增 `/aichat/status/...`。

#### 4.5.1 获取服务状态概览（API 总体）
`GET /aichat/status/summary`

Query:
- `from` (UTC "YYYY-MM-DD HH:MM:SS")
- `to` (UTC "YYYY-MM-DD HH:MM:SS")
- `interval`（可选，默认 `1m`）

Response:
```json
{
  "from": "2026-02-03 00:00:00",
  "to": "2026-02-03 01:00:00",
  "updated_at": "2026-02-03 01:00:05",
  "status": "OK",
  "availability": 1.0,
  "requests": 60,
  "success": 60,
  "avg_latency_ms": 230,
  "series": [
    {"bucket_start":"2026-02-03 00:00:00","requests":1,"success":1,"fail":0}
  ]
}
```

#### 4.5.2 渠道列表状态
`GET /aichat/status/channels`

Query:
- `from`, `to`, `interval`（同上）
- `include_series`（bool，默认 true；用于列表小柱状图）

Response:
```json
{
  "from": "...",
  "to": "...",
  "updated_at": "...",
  "items": [
    {
      "channel_id": 1,
      "channel_name": "gemini-business2api",
      "provider": "gemini",
      "status": "OK",
      "availability": 1.0,
      "requests": 60,
      "success": 60,
      "fail": 0,
      "avg_latency_ms": 210,
      "series": [
        {"bucket_start":"...","requests":2,"success":2,"fail":0}
      ]
    }
  ]
}
```

#### 4.5.3 单个渠道详情（可选）
`GET /aichat/status/channels/{channel_id}`

返回包含更丰富维度：top models、最近错误、p95 latency 等。

#### 4.5.4 模型列表状态
`GET /aichat/status/models`

Query:
- `from`, `to`, `interval`
- `provider`（可选）
- `channel_id`（可选：模型在不同渠道可表现不同）

Response:
```json
{
  "items": [
    {
      "provider": "gemini",
      "model": "Gemini 2.5 Flash",
      "status": "UNKNOWN",
      "availability": 1.0,
      "requests": 0,
      "success": 0,
      "fail": 0,
      "avg_latency_ms": null,
      "series": []
    }
  ]
}
```

#### 4.5.5 （可选）主动探测触发
`POST /aichat/status/probe/run`
Body:
```json
{ "entity_kind": "CHANNEL", "entity_key": "channel:1" }
```

用于管理员手动触发一次探测，返回 probe 结果。

### 4.6 权限与安全

建议：
- 这些监控接口仅供管理后台使用。
- 与现有认证机制对齐（若已有 token/session 体系则复用）。
- 若暂时没有统一 auth：可在 Nginx 层或后端加一个简单 `X-Admin-Token`（配置项），后续替换。

---

## 5. 前端设计（aiapi_web/）

### 5.1 路由与导航

新增左侧导航：
- “监控状态 / 服务状态”

建议路由：
- `/monitor/status`（服务状态）
- 未来扩展：`/monitor/errors`（已有 ErrorMonitor 可迁移到此）

### 5.2 页面结构（参考截图）

页面标题区：
- 标题：服务状态
- 子标题：最近更新：YYYY-MM-DD HH:mm:ss
- 右侧：刷新按钮、时间范围选择、间隔选择（可复用 ErrorMonitor 的 timeRange / interval 选择器）

模块 1：API 服务总览卡片
- 状态徽章：正常/异常/未知
- 指标：可用率、请求、成功、平均延迟
- 趋势：小柱状图（requests/success/fail 或 successRate）

模块 2：渠道状态列表
- 每行一个 channel
- 左：名称
- 中：可用率 / 请求 / 成功
- 下：小柱状图
- 右：状态徽章

模块 3：模型状态列表
- 每行一个 model（可按 provider 分组折叠）
- 同样指标与趋势

### 5.3 交互与筛选

必选：
- 时间范围：最近 1h / 6h / 24h / 7d
- 刷新：手动刷新

可选：
- 自动刷新：每 30s/60s
- 筛选：provider、channel、状态（OK/DOWN/UNKNOWN）
- 展开详情：点击行进入详情 drawer/modal

### 5.4 前端状态与数据流

- 页面加载：并发请求
  - `GET /aichat/status/summary`
  - `GET /aichat/status/channels`
  - `GET /aichat/status/models`
- 轮询刷新：可配置 60s；刷新时保留用户筛选与滚动位置。

### 5.5 TypeScript 类型（建议新增到 `src/types/index.ts`）

建议新增：
```ts
export type ServiceHealthStatus = 'OK' | 'DEGRADED' | 'DOWN' | 'UNKNOWN';

export interface StatusBucket {
  bucket_start: string; // backend UTC string
  requests: number;
  success: number;
  fail: number;
  avg_latency_ms?: number | null;
}

export interface ApiStatusSummary {
  from: string;
  to: string;
  updated_at: string;
  status: ServiceHealthStatus;
  availability: number;
  requests: number;
  success: number;
  avg_latency_ms?: number | null;
  series: StatusBucket[];
}

export interface ChannelStatusItem {
  channel_id: number;
  channel_name: string;
  provider?: string;
  status: ServiceHealthStatus;
  availability: number;
  requests: number;
  success: number;
  fail: number;
  avg_latency_ms?: number | null;
  series?: StatusBucket[];
}

export interface ModelStatusItem {
  provider?: string;
  model: string;
  channel_id?: number;
  status: ServiceHealthStatus;
  availability: number;
  requests: number;
  success: number;
  fail: number;
  avg_latency_ms?: number | null;
  series?: StatusBucket[];
}
```

### 5.6 组件设计

建议在 `src/components/` 新增：
- `ServiceStatusMonitor.tsx`（页面容器）
- `ServiceStatusMonitor.css`
- 子组件：
  - `StatusHeader`（标题+更新时间+工具栏）
  - `StatusRow`（单行：名称+指标+徽章+mini chart）
  - `MiniBars`（小柱状图，复用/改造 `SimpleBarChart` 支持 success/fail 堆叠或 successRate）

状态徽章：
- OK：绿色“正常”
- DEGRADED：橙色“波动”
- DOWN：红色“异常”
- UNKNOWN：灰色“未知”

### 5.7 UI 细节（对齐截图）

- 每条列表项：
  - 第一行：名称（左）+ 状态 pill（右）
  - 第二行：可用率 100%  请求 X  成功 Y
  - 第三行：mini bar chart（固定高度、固定桶数，如 60 个 1m 桶）

- 无数据：灰色 bars + “未知”
- 0 请求：展示请求 0、成功 0，可用率默认 100% 但状态 UNKNOWN（避免误导）

### 5.8 与时间参数的兼容

前端沿用我们在 ErrorMonitor 中引入的格式：
- 后端用 UTC 字符串 `YYYY-MM-DD HH:MM:SS`
- 前端保留 ISO Date 并通过 helper 转换为后端格式

---

## 6. 接口参数与示例

### 6.1 TimeRange
前端 timeRange → `from/to`：
- 1h：`to=now`, `from=now-1h`
- 6h：`from=now-6h`
- 24h：`from=now-24h`
- 7d：`from=now-7d`

### 6.2 interval 与桶数
推荐默认：
- 1h → 1m（60桶）
- 6h → 5m（72桶）
- 24h → 15m（96桶）
- 7d → 1h（168桶）

---

## 7. 性能与扩展

- 列表页要快：建议服务端返回已聚合数据，避免前端做重计算。
- `include_series=false` 可用于移动端/低带宽。
- 后端聚合表可以保留 30 天 1m 桶，超过降采样（例如 15m/1h），或使用分区表。

---

## 8. 研发计划（建议迭代）

### Iteration 1（最小可用）
1. 后端：实现 `GET /aichat/status/summary|channels|models`（基于现有请求/错误数据或临时聚合）。
2. 前端：新页面 + 列表 UI + 手动刷新。

### Iteration 2（增强）
1. 后端：引入 `aichat_status_buckets` 聚合表与定时聚合。
2. 前端：自动刷新、筛选、详情。

### Iteration 3（可选）
1. 后端：主动 probe 任务与 `aichat_probe_events`。
2. 前端：展示“探测状态/最近探测时间”。

---

## 9. 待确认问题（落地前需要你拍板）

1. **模型维度**：模型状态是"全局模型"（跨渠道）还是"模型×渠道"？
2. **数据来源**：目前后端是否已有"请求成功/失败"持久化表？（若没有，需要新增事件表或从日志解析）
3. **权限**：管理后台是否已有统一鉴权？若没有，是否接受临时 `X-Admin-Token`？
4. **是否启用主动探测**：默认关闭（节省成本）还是打开？

**用户回复：**
1. 模型状态维度按照【模型×渠道】
2. 现有数据来源，不确定，请检查
3. 管理端暂无用户管理，暂时不实现鉴权
4. 编写主动探测逻辑，但默认关闭（仅真实流量）

---

## 10. 数据来源检查结果

经过代码扫描，**后端已有请求/错误统计数据落库**：

### 10.1 现有数据表

| 表名 | 用途 | 关键字段 |
|------|------|----------|
| `request_agg_hour` | 请求聚合（按小时桶） | `bucket_start`, `provider`, `model`, `client_type`, `api_kind`, `stream`, `http_status`, `count` |
| `error_agg_hour` | 错误聚合（按小时桶） | `bucket_start`, `severity`, `domain`, `type`, `provider`, `model`, `client_type`, `api_kind`, `stream`, `http_status`, `count` |
| `error_event` | 错误明细 | `ts`, `severity`, `domain`, `type`, `provider`, `model`, `client_type`, `api_kind`, `stream`, `http_status`, `request_id`, `message`, ... |

### 10.2 数据采集路径

- [`GenerationService.cpp:334`](aiapi/src/sessionManager/GenerationService.cpp:334) 中的 `recordRequestCompletedStat()` 在请求完成时调用 `ErrorStatsService::recordRequestCompleted()`
- [`ErrorStatsService.cpp:193`](aiapi/src/metrics/ErrorStatsService.cpp:193) 将数据推入队列，后台线程 flush 到 `request_agg_hour` 表
- 错误/警告事件通过 `recordError()` / `recordWarn()` 写入 `error_event` + `error_agg_hour`

### 10.3 现有数据的局限

| 问题 | 影响 |
|------|------|
| **缺少 `channel_id` / `channel_name`** | 无法按渠道聚合；目前只有 `provider`（如 "chaynsapi"），但一个 provider 可能对应多个 channel |
| **缺少 `latency_ms`** | 无法计算平均延迟 |
| **缺少 `success` 布尔标记** | 需要通过 `http_status` 推断（2xx/3xx 视为成功） |

### 10.4 改进方案

为支持"模型×渠道"维度与延迟统计，需要：

1. **扩展 `RequestCompletedData` 结构**：增加 `channelId`, `channelName`, `latencyMs` 字段
2. **扩展 `request_agg_hour` 表**：增加 `channel_id`, `channel_name`, `total_latency_ms`, `success_count`, `fail_count` 列
3. **在 `GenerationService` 中采集**：记录请求开始时间，完成时计算延迟；从 session 中获取 channel 信息
4. **新增查询方法**：`queryStatusByChannel()`, `queryStatusByModel()` 支持按渠道/模型聚合

---

## 11. 下一步（落地顺序）

### Iteration 1（最小可用 - 基于现有数据）
1. **后端**：新增 3 个 status API（`/aichat/status/summary|channels|models`），基于现有 `request_agg_hour` + `error_agg_hour` 聚合
   - 暂时用 `provider` 代替 `channel`（因为目前只有一个 provider "chaynsapi"）
   - 成功/失败通过 `http_status` 推断
2. **前端**：新增 `ServiceStatusMonitor` 页面 + 路由

### Iteration 2（增强数据采集）
1. **后端**：扩展 `RequestCompletedData` + `request_agg_hour` 表，增加 `channel_id`, `channel_name`, `latency_ms`
2. **后端**：在 `GenerationService` 中采集 channel 信息与延迟
3. **前端**：展示延迟指标

### Iteration 3（主动探测）
1. **后端**：实现 `StatusProbeScheduler`，定时对渠道/模型发起探测
2. **后端**：新增 `aichat_probe_events` 表
3. **前端**：展示探测状态

