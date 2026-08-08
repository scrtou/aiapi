#ifndef DOMAIN_MODEL_SESSIONDATA_H
#define DOMAIN_MODEL_SESSIONDATA_H

#include <ctime>
#include <string>
#include <vector>
#include <json/json.h>

#include "domain/model/ImageInfo.h"
#include "domain/model/BridgeWireFormat.h"

/**
 * @file SessionData.h
 * @brief 会话的纯数据表示（session_st 及其枚举）
 *
 * 从 sessionManager/core/Session.h 抽出。原文件把纯数据 session_st 与带
 * DB 持久化/后台线程/读写锁的 chatSession 混装，导致 12 个只需要 session_st
 * 的文件（含 apipoint/APIinterface.h）被迫依赖整个 sessionManager。
 *
 * 本文件不含行为逻辑，不依赖 sessionManager 的任何头。
 */

/**
 * @brief 会话追踪模式
 *
 * 定义如何在连续对话中追踪会话上下文：
 * - Hash: 使用消息内容的 SHA256 哈希作为会话ID（原有方式）
 * - ZeroWidth: 使用零宽字符在响应末尾隐式嵌入会话ID（新方式）
 */
enum class SessionTrackingMode {
    Hash,       // 基于消息内容哈希的会话追踪（默认）
    ZeroWidth   // 基于零宽字符嵌入的会话追踪
};

/**
 * @brief API 类型枚举
 *
 * 区分不同的 API 接口类型，替代原来的 bool is_response_api
 */
enum class ApiType {
    ChatCompletions,  // 聊天补全接口（/v1/chat/completions）
    Responses         // 响应接口（/v1/responses）
};

static const int SESSION_EXPIRE_TIME = 86400; // 24小时，单位秒数，会话过期时间
static const int SESSION_CLEANUP_INTERVAL = 3600; // 1小时，单位秒数，过期会话轮询清理间隔（须远小于 SESSION_EXPIRE_TIME）
// 会话_MAX_MESSAGES = 4; //上下文会话最大消息条数,一轮两条
// Image信息 定义在 Generation请求. 中
struct session_st
{
  struct RequestData {
    /// 本次请求选择的上游通道标识（例如 chaynsapi / openai），用于路由到具体 provider。
    std::string api="";
    /// 本次请求目标模型名（如 GPT-4o、gpt-4o-mini），用于上游请求体构建。
    std::string model="";
    /// 系统提示词，影响本轮及后续补全行为；为空时表示不注入 system 消息。
    std::string systemPrompt="";
    /// 当前轮用户输入文本（标准化后），作为本轮主要 user message 内容。
    std::string message="";
    /// 当前轮解析出的图片输入列表，供上游在多模态场景上传或拼装请求。
    std::vector<ImageInfo> images;
    /// 标准化后的工具定义数组（当前轮有效），用于 tool calling。
    Json::Value tools;
    /// 原始工具定义快照（尽量保真），用于桥接/降级场景兜底。
    Json::Value toolsRaw;
    /// 工具选择策略（auto/none/required 或对象 JSON 字符串），直接映射客户端语义。
    std::string toolChoice="";
    /// 是否允许同一轮返回多个工具调用；Codex 由 parallel_tool_calls 控制。
    bool parallelToolCalls = true;
    /// 原始用户输入文本（保留零宽字符/特殊标记），用于会话连续性与追踪解析。
    std::string rawMessage="";
  };

  struct ResponseData {
    /// 上游返回的标准化响应主体（含文本、错误码等关键信息）。
    Json::Value message;
    /// 上游完整响应数据（用于 Responses 协议 GET 查询、审计与排障）。
    Json::Value apiData;
    /// 当前响应对应的 responseId（Responses 协议主键）。
    std::string responseId="";
    /// 上一轮 responseId（用于 previous_response_id 串联续聊）。
    std::string lastResponseId="";
  };

  struct SessionState {
    /// 当前会话所属 API 类型（ChatCompletions / Responses），决定分支处理逻辑。
    ApiType apiType = ApiType::ChatCompletions;
    /// 请求是否携带 previous_response_id，用于 Responses 续聊路径判定。
    bool hasPreviousResponseId = false;
    /// 本轮是否命中历史会话并进入续聊流程（新建为 false，续聊为 true）。
    bool isContinuation = false;
    /// 当前会话主键（session_map key），是会话读写与并发门控的核心标识。
    std::string conversationId = "";
    /// 两阶段转移预生成的下一轮会话 ID；响应发送后提交转移时消费。
    std::string nextSessionId = "";
    /// 会话首次创建时间戳（秒），用于过期清理与生命周期统计。
    time_t createdAt = 0;
    /// 最近活跃时间戳（秒），每轮请求/更新后刷新，用于闲置淘汰。
    time_t lastActiveAt = 0;
    /// 请求链路 ID（若有），用于日志关联与错误追踪。
    std::string requestId = "";
    /// Hash 追踪模式下的上下文键（context_map key），用于跨轮映射真实会话。
    std::string contextConversationId = "";
    /// 当前上下文窗口长度（消息条目计数），用于裁剪与 key 生成策略。
    int contextLength = 0;
    /// 上下文是否已到裁剪边界；为 true 时会走“满窗口”分支逻辑。
    bool contextIsFull = false;
  };

  struct ProviderContext {
    /// provider 线程上下文前一轮键（例如 threadId 映射的查找键），用于线程复用/迁移。
    std::string prevProviderKey = "";
    /// 工具桥接触发信号（随机哨兵串），用于识别与清洗桥接注入内容。
    std::string toolBridgeTrigger = "";
    /// 本次请求已解析并固定的文本桥格式；不得在响应阶段重新猜测配置。
    toolcall::BridgeWireFormat toolBridgeFormat = toolcall::BridgeWireFormat::Unset;
    /// 迁移期开关；默认禁止响应解析跨格式回退。
    bool toolBridgeAllowFormatFallback = false;
    /// 当前 provider 是否支持原生工具调用；影响工具输出格式与桥接策略。
    bool supportsToolCalls = true;
    /// 客户端元信息（client_type、client_version 等），用于规则分流与兼容策略。
    Json::Value clientInfo;
    /// 历史消息上下文数组（role/content），作为续聊时上游输入的一部分。
    Json::Value messageContext = Json::Value(Json::arrayValue);
  };

  RequestData request;
  ResponseData response;
  SessionState state;
  ProviderContext provider;

  void clearMessageContext()
  {
    provider.messageContext.clear();
  }

  void addMessageToContext(const Json::Value& message)
  {
    provider.messageContext.append(message);
  }

  bool isResponseApi() const { return state.apiType == ApiType::Responses; }
  bool isChatApi() const { return state.apiType == ApiType::ChatCompletions; }
};

#endif // DOMAIN_MODEL_SESSIONDATA_H
