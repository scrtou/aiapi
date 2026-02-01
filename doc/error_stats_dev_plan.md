# aiapi 错误统计与监控 - 开发计划

版本：v1.0
日期：2026-02-01
关联设计文档：[aiapi 错误统计与监控方案（设计文档）](./aiapi%20错误统计与监控方案（设计文档）)

---

## 开发阶段总览

| Phase | 名称 | 预估工作量 | 依赖 | 交付物 |
|-------|------|-----------|------|--------|
| 1 | 基础模型与配置 | 小 | 无 | ErrorEvent.h, ErrorStatsConfig.h |
| 2 | DB Manager | 中 | Phase 1 | ErrorStatsDbManager.h/.cpp |
| 3 | ErrorStatsService | 中 | Phase 1, 2 | ErrorStatsService.h/.cpp |
| 4 | 采集接入 | 大 | Phase 3 | GenerationService 改动 |
| 5 | API Endpoints | 中 | Phase 2, 3 | Controller 新增接口 |
| 6 | 清理任务 | 小 | Phase 2 | 定时清理逻辑 |
| 7 | 测试与验证 | 中 | 全部 | 单元测试 + 集成测试 |

---

## Phase 1: 基础模型与配置

### 1.1 目标
定义错误事件的数据结构、枚举常量、配置项加载逻辑。

### 1.2 新增文件
```
aiapi/src/metrics/
├── ErrorEvent.h          # 事件结构体 + 枚举
├── ErrorStatsConfig.h    # 配置结构体
└── ErrorStatsConfig.cpp  # 配置加载（从 custom_config.error_stats）
```

### 1.3 详细设计

#### 1.3.1 ErrorEvent.h
```cpp
#ifndef ERROR_EVENT_H
#define ERROR_EVENT_H

#include <string>
#include <chrono>
#include <json/json.h>

namespace metrics {

// 事件等级
enum class Severity {
    WARN,
    ERROR
};

// 事件域
enum class Domain {
    UPSTREAM,
    TOOL_BRIDGE,
    TOOL_VALIDATION,
    SESSION_GATE,
    INTERNAL,
    REQUEST
};

// 事件类型（字符串常量，便于扩展）
namespace EventType {
    // UPSTREAM
    constexpr const char* UPSTREAM_NETWORK_ERROR = "upstream.network_error";
    constexpr const char* UPSTREAM_TIMEOUT = "upstream.timeout";
    constexpr const char* UPSTREAM_RATE_LIMITED = "upstream.rate_limited";
    constexpr const char* UPSTREAM_AUTH_ERROR = "upstream.auth_error";
    constexpr const char* UPSTREAM_HTTP_ERROR = "upstream.http_error";
    constexpr const char* UPSTREAM_SERVICE_UNAVAILABLE = "upstream.service_unavailable";
    
    // TOOL_BRIDGE
    constexpr const char* TOOLBRIDGE_TRANSFORM_INJECTED = "toolbridge.transform_injected";
    constexpr const char* TOOLBRIDGE_TRIGGER_MISSING = "toolbridge.trigger_missing";
    constexpr const char* TOOLBRIDGE_TRIGGER_MISMATCH_FALLBACK = "toolbridge.trigger_mismatch_fallback";
    constexpr const char* TOOLBRIDGE_XML_NOT_FOUND = "toolbridge.xml_not_found";
    constexpr const char* TOOLBRIDGE_XML_PARSE_ERROR = "toolbridge.xml_parse_error";
    constexpr const char* TOOLBRIDGE_SENTINEL_MISMATCH = "toolbridge.sentinel_mismatch";
    constexpr const char* TOOLBRIDGE_ARGS_JSON_PARSE_ERROR = "toolbridge.args_json_parse_error";
    constexpr const char* TOOLBRIDGE_NORMALIZE_APPLIED = "toolbridge.normalize_applied";
    constexpr const char* TOOLBRIDGE_FORCED_TOOLCALL_GENERATED = "toolbridge.forced_toolcall_generated";
    constexpr const char* TOOLBRIDGE_VALIDATION_FILTERED = "toolbridge.validation_filtered";
    constexpr const char* TOOLBRIDGE_VALIDATION_FALLBACK_APPLIED = "toolbridge.validation_fallback_applied";
    constexpr const char* TOOLBRIDGE_SELFHEAL_READ_FILE_APPLIED = "toolbridge.selfheal_read_file_applied";
    constexpr const char* TOOLBRIDGE_STRICT_CLIENT_RULE_APPLIED = "toolbridge.strict_client_rule_applied";
    
    // TOOL_VALIDATION
    constexpr const char* TOOLVALIDATION_TOOL_NOT_FOUND = "toolvalidation.tool_not_found";
    constexpr const char* TOOLVALIDATION_ARGUMENTS_NOT_OBJECT = "toolvalidation.arguments_not_object";
    constexpr const char* TOOLVALIDATION_REQUIRED_FIELD_MISSING = "toolvalidation.required_field_missing";
    constexpr const char* TOOLVALIDATION_FIELD_TYPE_MISMATCH = "toolvalidation.field_type_mismatch";
    constexpr const char* TOOLVALIDATION_CRITICAL_FIELD_EMPTY = "toolvalidation.critical_field_empty";
    
    // SESSION_GATE
    constexpr const char* SESSIONGATE_REJECTED_CONFLICT = "sessiongate.rejected_conflict";
    constexpr const char* SESSIONGATE_CANCELLED = "sessiongate.cancelled";
    
    // INTERNAL
    constexpr const char* INTERNAL_EXCEPTION = "internal.exception";
    constexpr const char* INTERNAL_UNKNOWN = "internal.unknown";
}

// 错误事件结构体
struct ErrorEvent {
    int64_t id = 0;                                    // DB 自增 ID（写入后填充）
    std::chrono::system_clock::time_point ts;         // 事件时间（UTC）
    Severity severity = Severity::WARN;
    Domain domain = Domain::INTERNAL;
    std::string type;                                  // 事件类型（EventType::*）
    
    // 聚合维度
    std::string provider;
    std::string model;
    std::string clientType;
    std::string apiKind;                               // "chat_completions" | "responses"
    bool stream = false;
    int httpStatus = 0;
    
    // 请求级字段
    std::string requestId;
    std::string responseId;                            // 可选
    std::string toolName;                              // 可选
    
    // 详情
    std::string message;                               // 简短信息
    Json::Value detailJson;                            // 结构化详情
    std::string rawSnippet;                            // 原始片段（可选）
    
    // 辅助方法
    static std::string severityToString(Severity s);
    static Severity stringToSeverity(const std::string& s);
    static std::string domainToString(Domain d);
    static Domain stringToDomain(const std::string& s);
};

// 请求完成事件（用于 request_agg_hour）
struct RequestCompletedEvent {
    std::chrono::system_clock::time_point ts;
    std::string provider;
    std::string model;
    std::string clientType;
    std::string apiKind;
    bool stream = false;
    int httpStatus = 0;
};

} // namespace metrics

#endif // ERROR_EVENT_H
```

#### 1.3.2 ErrorStatsConfig.h
```cpp
#ifndef ERROR_STATS_CONFIG_H
#define ERROR_STATS_CONFIG_H

#include <string>
#include <cstdint>

namespace metrics {

enum class DropPolicy {
    DROP_OLDEST,
    DROP_NEWEST
};

struct ErrorStatsConfig {
    bool enabled = true;
    bool persistDetail = true;
    bool persistAgg = true;
    bool persistRequestAgg = true;
    
    int retentionDaysDetail = 30;
    int retentionDaysAgg = 30;
    int retentionDaysRequestAgg = 30;
    
    bool rawSnippetEnabled = true;
    size_t rawSnippetMaxLen = 32768;  // 32KB
    
    size_t asyncBatchSize = 200;
    size_t asyncFlushMs = 200;
    DropPolicy dropPolicy = DropPolicy::DROP_OLDEST;
    
    size_t queueCapacity = 10000;     // 队列最大容量
    
    // 从 Json::Value 加载配置
    static ErrorStatsConfig loadFromJson(const Json::Value& config);
    
    // 获取全局配置实例
    static ErrorStatsConfig& getInstance();
};

} // namespace metrics

#endif // ERROR_STATS_CONFIG_H
```

### 1.4 CMakeLists.txt 改动
```cmake
# 新增 metrics 目录
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/metrics)

# 新增源文件
add_executable(${PROJECT_NAME}
    ...
    metrics/ErrorStatsConfig.cpp
)
```

---

## Phase 2: DB Manager

### 2.1 目标
实现数据库表的创建、升级、批量写入、聚合 upsert、查询接口。

### 2.2 新增文件
```
aiapi/src/dbManager/metrics/
├── ErrorStatsDbManager.h
└── ErrorStatsDbManager.cpp
```

### 2.3 详细设计

#### 2.3.1 表结构 SQL（PostgreSQL）

```sql
-- error_event 明细表
CREATE TABLE IF NOT EXISTS error_event (
    id BIGSERIAL PRIMARY KEY,
    ts TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    severity VARCHAR(8) NOT NULL,
    domain VARCHAR(32) NOT NULL,
    type VARCHAR(64) NOT NULL,
    provider VARCHAR(64) NOT NULL DEFAULT '',
    model VARCHAR(64) NOT NULL DEFAULT '',
    client_type VARCHAR(64) NOT NULL DEFAULT '',
    api_kind VARCHAR(32) NOT NULL DEFAULT '',
    stream BOOLEAN NOT NULL DEFAULT FALSE,
    http_status INTEGER NOT NULL DEFAULT 0,
    request_id VARCHAR(128) NOT NULL DEFAULT '',
    response_id VARCHAR(128) DEFAULT '',
    tool_name VARCHAR(128) DEFAULT '',
    message TEXT NOT NULL DEFAULT '',
    detail_json JSONB DEFAULT '{}',
    raw_snippet TEXT DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_error_event_ts ON error_event(ts DESC);
CREATE INDEX IF NOT EXISTS idx_error_event_filter ON error_event(domain, type, severity, provider, model, client_type, ts DESC);
CREATE INDEX IF NOT EXISTS idx_error_event_request ON error_event(request_id);

-- error_agg_hour 聚合表
CREATE TABLE IF NOT EXISTS error_agg_hour (
    bucket_start TIMESTAMPTZ NOT NULL,
    severity VARCHAR(8) NOT NULL,
    domain VARCHAR(32) NOT NULL,
    type VARCHAR(64) NOT NULL,
    provider VARCHAR(64) NOT NULL DEFAULT '',
    model VARCHAR(64) NOT NULL DEFAULT '',
    client_type VARCHAR(64) NOT NULL DEFAULT '',
    api_kind VARCHAR(32) NOT NULL DEFAULT '',
    stream BOOLEAN NOT NULL DEFAULT FALSE,
    http_status INTEGER NOT NULL DEFAULT 0,
    count BIGINT NOT NULL DEFAULT 0,
    last_event_ts TIMESTAMPTZ NOT NULL,
    PRIMARY KEY (bucket_start, severity, domain, type, provider, model, client_type, api_kind, stream, http_status)
);

CREATE INDEX IF NOT EXISTS idx_error_agg_hour_bucket ON error_agg_hour(bucket_start DESC);

-- request_agg_hour 请求聚合表
CREATE TABLE IF NOT EXISTS request_agg_hour (
    bucket_start TIMESTAMPTZ NOT NULL,
    provider VARCHAR(64) NOT NULL DEFAULT '',
    model VARCHAR(64) NOT NULL DEFAULT '',
    client_type VARCHAR(64) NOT NULL DEFAULT '',
    api_kind VARCHAR(32) NOT NULL DEFAULT '',
    stream BOOLEAN NOT NULL DEFAULT FALSE,
    http_status INTEGER NOT NULL DEFAULT 0,
    count BIGINT NOT NULL DEFAULT 0,
    last_request_ts TIMESTAMPTZ NOT NULL,
    PRIMARY KEY (bucket_start, provider, model, client_type, api_kind, stream, http_status)
);

CREATE INDEX IF NOT EXISTS idx_request_agg_hour_bucket ON request_agg_hour(bucket_start DESC);
```

#### 2.3.2 ErrorStatsDbManager 接口
```cpp
class ErrorStatsDbManager {
public:
    static std::shared_ptr<ErrorStatsDbManager> getInstance();
    
    // 初始化（建表/升级）
    void init();
    bool isTableExist(const std::string& tableName);
    void createTables();
    void checkAndUpgradeTables();
    
    // 批量写入
    void insertEvents(const std::vector<ErrorEvent>& events);
    void upsertErrorAggHour(const std::vector<ErrorEvent>& events);
    void upsertRequestAggHour(const std::vector<RequestCompletedEvent>& events);
    
    // 查询接口
    std::vector<Json::Value> queryErrorSeries(
        const std::string& from, const std::string& to,
        const std::map<std::string, std::string>& filters);
    
    std::vector<Json::Value> queryRequestSeries(
        const std::string& from, const std::string& to,
        const std::map<std::string, std::string>& filters);
    
    std::vector<ErrorEvent> queryEvents(
        const std::string& from, const std::string& to,
        const std::map<std::string, std::string>& filters,
        int limit, int offset);
    
    std::optional<ErrorEvent> queryEventById(int64_t id);
    
    // 清理
    void deleteOldEvents(int retentionDays);
    void deleteOldErrorAgg(int retentionDays);
    void deleteOldRequestAgg(int retentionDays);
    
private:
    std::shared_ptr<drogon::orm::DbClient> dbClient_;
    DbType dbType_ = DbType::PostgreSQL;
    
    void detectDbType();
    std::string getBucketStartSql();  // 根据 DB 类型返回对齐整点的 SQL
};
```

### 2.4 CMakeLists.txt 改动
```cmake
# 新增 dbManager/metrics 目录
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/dbManager/metrics)

# 新增源文件
add_executable(${PROJECT_NAME}
    ...
    dbManager/metrics/ErrorStatsDbManager.cpp
)
```

---

## Phase 3: ErrorStatsService

### 3.1 目标
实现异步写入服务：有界队列、后台 flush 线程、Prometheus 指标更新。

### 3.2 新增文件
```
aiapi/src/metrics/
├── ErrorStatsService.h
└── ErrorStatsService.cpp
```

### 3.3 详细设计

#### 3.3.1 ErrorStatsService 接口
```cpp
class ErrorStatsService {
public:
    static ErrorStatsService& getInstance();
    
    // 初始化（启动后台线程）
    void init();
    void shutdown();
    
    // 记录事件（线程安全，非阻塞）
    void recordEvent(ErrorEvent event);
    void recordWarn(Domain domain, const std::string& type, const std::string& message,
                    const session_st& session, const Json::Value& detail = Json::nullValue,
                    const std::string& rawSnippet = "");
    void recordError(Domain domain, const std::string& type, const std::string& message,
                     const session_st& session, int httpStatus = 0,
                     const Json::Value& detail = Json::nullValue,
                     const std::string& rawSnippet = "");
    
    // 记录请求完成（用于 request_agg_hour）
    void recordRequestCompleted(const session_st& session, int httpStatus);
    
    // 获取统计（用于调试）
    size_t getQueueSize() const;
    uint64_t getDroppedCount() const;
    
private:
    ErrorStatsService() = default;
    ~ErrorStatsService();
    
    // 队列
    std::deque<ErrorEvent> eventQueue_;
    std::deque<RequestCompletedEvent> requestQueue_;
    mutable std::mutex queueMutex_;
    std::condition_variable queueCv_;
    
    // 后台线程
    std::thread flushThread_;
    std::atomic<bool> running_{false};
    
    // 统计
    std::atomic<uint64_t> droppedCount_{0};
    
    // 内部方法
    void flushLoop();
    void flushBatch();
    void updatePromMetrics(const ErrorEvent& event);
    void updatePromRequestMetrics(const RequestCompletedEvent& event);
    ErrorEvent buildEvent(Severity severity, Domain domain, const std::string& type,
                          const std::string& message, const session_st& session,
                          int httpStatus, const Json::Value& detail,
                          const std::string& rawSnippet);
};
```

#### 3.3.2 Prometheus 指标
```cpp
// 在 ErrorStatsService.cpp 中注册指标
// 使用 drogon::plugin::PromExporter 的 API

// aiapi_error_events_total{severity, domain, type, provider, model, client_type, api_kind, stream}
// aiapi_requests_total{provider, model, client_type, api_kind, stream, http_status}
// aiapi_error_events_dropped_total{reason}
```

---

## Phase 4: 采集接入

### 4.1 目标
在 GenerationService 的关键路径上调用 ErrorStatsService 记录事件。

### 4.2 改动文件
- `aiapi/src/sessionManager/GenerationService.cpp`
- `aiapi/src/sessionManager/GenerationService.h`
- `aiapi/src/sessionManager/Session.h`（新增 request_id 字段）
- `aiapi/src/controllers/AiApi.cc`（生成 request_id）

### 4.3 详细改动点

#### 4.3.1 Session.h 新增字段
```cpp
struct session_st {
    // ... 现有字段 ...
    
    // 新增：请求唯一 ID（用于错误追踪）
    std::string request_id;
};
```

#### 4.3.2 AiApi.cc 生成 request_id
```cpp
// 在 handleChatCompletions / handleResponses 入口处
std::string requestId = req->getHeader("x-request-id");
if (requestId.empty()) {
    requestId = drogon::utils::getUuid();  // 或自定义 UUID 生成
}
// 传递给 GenerationRequest
```

#### 4.3.3 GenerationService 采集点

| 位置 | 事件类型 | 等级 | 触发条件 |
|------|---------|------|---------|
| executeProvider | upstream.* | ERROR | !result.isSuccess() |
| executeGuardedWithSession | sessiongate.rejected_conflict | ERROR | guard.getResult()==Rejected |
| executeGuardedWithSession | sessiongate.cancelled | WARN | guard.isCancelled() |
| emitResultEvents | toolbridge.xml_not_found | WARN | xmlInput.empty() |
| emitResultEvents | toolbridge.normalize_applied | WARN | normalized != original |
| emitResultEvents | toolbridge.xml_parse_error | WARN | parseXmlToolCalls 失败 |
| emitResultEvents | toolbridge.trigger_missing | WARN | expectedSentinel.empty() |
| emitResultEvents | toolbridge.sentinel_mismatch | WARN | sentinel 不匹配 |
| emitResultEvents | toolbridge.forced_toolcall_generated | WARN | generateForcedToolCall 触发 |
| emitResultEvents | toolbridge.validation_filtered | WARN | removedCount > 0 |
| emitResultEvents | toolbridge.validation_fallback_applied | WARN | applyValidationFallback 调用 |
| emitResultEvents | toolbridge.selfheal_read_file_applied | WARN | selfHealReadFile 触发 |
| emitResultEvents | toolbridge.strict_client_rule_applied | WARN | applyStrictClientRules 修改 |
| executeGuardedWithSession catch | internal.exception | ERROR | std::exception |
| executeGuardedWithSession catch | internal.unknown | ERROR | ... |
| 请求结束 | (recordRequestCompleted) | - | 每个请求 |

---

## Phase 5: API Endpoints

### 5.1 目标
在 AiApi Controller 中新增统计查询接口。

### 5.2 改动文件
- `aiapi/src/controllers/AiApi.h`
- `aiapi/src/controllers/AiApi.cc`

### 5.3 新增接口

#### 5.3.1 GET /aichat/metrics/requests/series
```cpp
void getRequestsSeries(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback);
```

#### 5.3.2 GET /aichat/metrics/errors/series
```cpp
void getErrorsSeries(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);
```

#### 5.3.3 GET /aichat/metrics/errors/events
```cpp
void getErrorsEvents(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);
```

#### 5.3.4 GET /aichat/metrics/errors/events/{id}
```cpp
void getErrorsEventById(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback,
                        int64_t id);
```

### 5.4 AiApi.h METHOD_LIST 新增
```cpp
METHOD_LIST_BEGIN
    // ... 现有方法 ...
    
    // 错误统计 API
    METHOD_ADD(AiApi::getRequestsSeries, "/aichat/metrics/requests/series", Get);
    METHOD_ADD(AiApi::getErrorsSeries, "/aichat/metrics/errors/series", Get);
    METHOD_ADD(AiApi::getErrorsEvents, "/aichat/metrics/errors/events", Get);
    METHOD_ADD(AiApi::getErrorsEventById, "/aichat/metrics/errors/events/{id}", Get);
METHOD_LIST_END
```

---

## Phase 6: 清理任务

### 6.1 目标
实现定时清理过期数据的逻辑。

### 6.2 实现方式
在 ErrorStatsService 中启动一个定时任务（每小时执行一次），调用 ErrorStatsDbManager 的 delete 方法。

```cpp
// ErrorStatsService::init() 中
drogon::app().getLoop()->runEvery(std::chrono::hours(1), [this]() {
    auto& config = ErrorStatsConfig::getInstance();
    auto dbManager = ErrorStatsDbManager::getInstance();
    
    dbManager->deleteOldEvents(config.retentionDaysDetail);
    dbManager->deleteOldErrorAgg(config.retentionDaysAgg);
    dbManager->deleteOldRequestAgg(config.retentionDaysRequestAgg);
    
    LOG_INFO << "Error stats retention cleanup completed";
});
```

---

## Phase 7: 测试与验证

### 7.1 单元测试
- ErrorEvent 序列化/反序列化
- ErrorStatsConfig 加载
- ErrorStatsDbManager 批量写入/聚合 upsert
- ErrorStatsService 队列满丢弃策略

### 7.2 集成测试
- 模拟 ToolBridge WARN 路径，验证 DB + Prom 记录
- 模拟 Provider ERROR 路径，验证 DB + Prom 记录
- 验证 API 返回数据正确性
- 验证清理任务正常执行

### 7.3 测试文件
```
aiapi/src/test/
├── test_error_event.cpp
├── test_error_stats_config.cpp
├── test_error_stats_db.cpp
└── test_error_stats_service.cpp
```

---

## 实施顺序与依赖关系

```
Phase 1 (基础模型与配置)
    │
    ├──► Phase 2 (DB Manager)
    │        │
    │        └──► Phase 6 (清理任务)
    │
    └──► Phase 3 (ErrorStatsService)
             │
             ├──► Phase 4 (采集接入)
             │
             └──► Phase 5 (API Endpoints)
                      │
                      └──► Phase 7 (测试与验证)
```

---

## 风险与注意事项

1. **性能影响**：异步写入 + 批量 upsert 可控制影响；但需监控队列积压情况
2. **DB 兼容性**：PostgreSQL 优先，SQLite 需要调整部分 SQL 语法（如 UPSERT）
3. **Prom 标签基数**：严格控制标签数量，避免高基数导致 Prom 内存爆炸
4. **并发安全**：ErrorStatsService 的队列操作需要加锁
5. **配置热更新**：v1 不支持热更新，需重启生效

---

## 预估工时

| Phase | 预估工时 |
|-------|---------|
| Phase 1 | 2h |
| Phase 2 | 4h |
| Phase 3 | 4h |
| Phase 4 | 6h |
| Phase 5 | 3h |
| Phase 6 | 1h |
| Phase 7 | 4h |
| **总计** | **24h** |

---

## 下一步

确认开发计划后，从 Phase 1 开始实施。
