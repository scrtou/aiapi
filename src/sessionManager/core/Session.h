#ifndef SESSION_H
#define SESSION_H
#include <string>
#include <map>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <json/json.h>
#include <sstream>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/err.h>
#include <iomanip>
#include <stdexcept>
#include <deque>
#include <list>
#include <memory>
#include <thread>
#include <atomic>

// 前向声明 类型，避免在头文件中直接
namespace drogon {
    class HttpRequest;
    using HttpRequestPtr = std::shared_ptr<HttpRequest>;
}

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
// 会话_MAX_MESSAGES = 4; //上下文会话最大消息条数,一轮两条
// Image信息 定义在 Generation请求. 中
#include "sessionManager/contracts/GenerationRequest.h"
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
class chatSession
{
  private:
    chatSession();
    ~chatSession();
    static chatSession *instance;
    std::mutex mutex_;
    std::unordered_map<std::string, session_st> session_map;
    std::unordered_map<std::string, std::string> context_map;//上下文会话id与会话id的映射
    SessionTrackingMode trackingMode_ = SessionTrackingMode::Hash;  // 默认使用Hash模式
    std::atomic<bool> stopClearExpiredLoop_{false};
    std::thread clearExpiredThread_;
public:
    static chatSession *getInstance()
    {
        static chatSession instance;
        return &instance;
    }
    
    // ========== 会话追踪模式相关方法 ==========
    /**
     * @brief 设置会话追踪模式
     * @param mode 追踪模式
     */
    void setTrackingMode(SessionTrackingMode mode) { trackingMode_ = mode; }
    
    /**
     * @brief 获取当前会话追踪模式
     * @return 当前追踪模式
     */
    SessionTrackingMode getTrackingMode() const { return trackingMode_; }
    
    /**
     * @brief 检查是否使用零宽字符追踪模式
     * @return true 如果使用零宽字符模式
     */
    bool isZeroWidthMode() const { return trackingMode_ == SessionTrackingMode::ZeroWidth; }
    
    // ========== 基础会话操作方法 ==========
    void addSession(const std::string &ConversationId,session_st &session);
    void delSession(const std::string &ConversationId);
    void getSession(const std::string &ConversationId, session_st &session);
    void updateSession(const std::string &ConversationId,session_st &session);
    void clearExpiredSession();
    void startClearExpiredSession();
    bool sessionIsExist(const std::string &ConversationId);
    bool sessionIsExist(session_st &sessio);

    /**
     * @brief Hash 模式：消费一次 context_map 映射
     *
     * 当 Hash key 未命中 session_map，但命中 context_map 时，说明处于“上下文裁剪”边界。
     * 该方法会：
     * - 返回映射后的真实 sessionId
     * - 删除 context_map 中对应项（一次性映射）
     * - 将目标会话标记为 contextIsFull=true（保持旧行为）
     */
    bool consumeContextMapping(const std::string& contextConversationId, std::string& outSessionId);

    void coverSessionresponse(session_st& session);
    static std::string generateConversationKey(const Json::Value& keyData);
    static std::string generateSHA256(const std::string& input);
    
    // ========== 会话创建/更新方法（外层包装，含模式判断）==========
    /**
     * @brief [Chat API] 创建或更新会话
     *
     * 根据当前配置的追踪模式自动选择：
     * - Hash模式：调用 createOrUpdateSessionHash()
     * - 零宽字符模式：调用 createOrUpdateSessionZeroWidth()
     *
     * @param session 会话对象（会被修改）
     * @return 处理后的会话引用
     */
    session_st& createOrUpdateChatSession(session_st& session);
    
    /**
     * @brief [Response API] 创建或更新会话
     *
     * 按优先级处理：
     * 1. 如果有 previous_response_id → 调用 createOrUpdateSessionByPreviousResponseId()
     * 2. 否则根据配置的追踪模式选择 Hash 或 ZeroWidth 方法
     *
     * @param session 会话对象（会被修改）
     * @return 处理后的会话引用
     */
    session_st& createOrUpdateResponseSession(session_st& session);

    /**
     * @brief 按 sessionId 获取或创建会话（新主路径）
     *
     * GenerationService 在完成会话连续性决策后，使用该方法：
     * - 若 sessionId 已存在：合并本次请求字段到存量会话（保留 messageContext）
     * - 若 sessionId 不存在：创建新会话
     *
     * 该方法会设置：
     * - session.state.conversationId = sessionId
     * - session.provider.prevProviderKey = sessionId（用于 provider thread map 查找）
     * - session.state.isContinuation
     */
    session_st& getOrCreateSession(const std::string& sessionId, session_st& session);
    
    // ========== 会话创建/更新方法（底层实现，独立功能）==========
    /**
     * @brief [Hash模式] 基于消息内容哈希创建或更新会话
     *
     * 功能：
     * - 根据消息内容生成会话ID（SHA256哈希）
     * - 如果会话存在则更新，否则创建新会话
     * - 支持 context_map 的上下文映射查找
     *
     * @param session 会话对象（会被修改）
     * @return 处理后的会话引用
     */
    session_st& createOrUpdateSessionHash(session_st& session);
    
    /**
     * @brief [Previous Response ID模式] 基于previous_response_id延续会话
     *
     * 功能：
     * - 根据 session.state.conversationId（已从previous_response_id解析）查找会话
     * - 如果会话存在则更新并延续上下文
     * - 如果会话不存在则创建新会话
     *
     * @param session 会话对象（会被修改，需要预先设置 conversationId）
     * @return 处理后的会话引用
     */
    session_st& createOrUpdateSessionByPreviousResponseId(session_st& session);

    Json::Value generateJsonbySession(const session_st& session,bool contextIsFull);
    
    // ========== 新增方法（响应 API 使用）==========
    // 生成唯一的 响应_id (resp_xxx 格式)
    static std::string generateResponseId();
    
    /**
     * @brief 根据 API 类型和追踪模式生成 conversationId
     *
     * 生成规则：
     * - Response API + hasPreviousResponseId → "resp_xxx" 格式
     * - Response API + ZeroWidth → "zw_xxx" 格式
     * - Response API + Hash → "" (延迟生成)
     * - Chat API + ZeroWidth → "zw_xxx" 格式
     * - Chat API + Hash → "" (延迟生成)
     *
     * @param apiType API 类型
     * @param mode 会话追踪模式
     * @param hasPreviousResponseId 是否携带 previous_response_id
     * @return 生成的会话ID，空字符串表示需要延迟生成（Hash模式）
     */
    static std::string generateCurConversationId(ApiType apiType, SessionTrackingMode mode, bool hasPreviousResponseId);
    
    // 为 响应 API 创建会话（使用 响应_id 作为键）
    std::string createResponseSession(session_st& session);
    
    // 通过 响应_id 获取会话
    bool getResponseSession(const std::string& responseId, session_st& session);
    
    // 通过 响应_id 删除会话
    bool deleteResponseSession(const std::string& responseId);
    
    // 更新 响应 API 会话（不删除旧 会话，直接更新）
    void updateResponseSession(session_st& session);

    // 更新/写入 响应 API 的完整响应数据（用于 GET /Responses/{}）。
    // 只更新 api_响应_data，不覆盖 messageContext 等上下文字段。
    bool updateResponseApiData(const std::string& responseId, const Json::Value& apiData);

    // ========== 零宽字符追踪模式方法 ==========
    /**
     * @brief 生成用于零宽字符模式的唯一会话ID
     * @return 格式为 "zw_timestamp_random" 的会话ID
     */
    static std::string generateZeroWidthSessionId();
    
    /**
     * @brief 在零宽字符模式下创建或更新会话
     * 
     * 该方法会根据零宽字符中的会话ID决定延续或新建会话：
     * 1. 先从请求消息中提取嵌入的会话ID
     * 2. 如果找到，则更新现有会话
     * 3. 如果未找到，则创建新会话并生成新的会话ID
     * 
     * @param session 会话对象（会被修改）
     * @return 处理后的会话引用
     */
    session_st& createOrUpdateSessionZeroWidth(session_st& session);
    
    /**
     * @brief 从消息文本中提取嵌入的会话ID
     * 
     * @param text 包含零宽字符的文本
     * @return 提取的会话ID，如果未找到返回空字符串
     */
    static std::string extractSessionIdFromText(const std::string& text);
    
    /**
     * @brief 从消息文本中提取并移除嵌入的会话ID
     * 
     * @param text 包含零宽字符的文本（会被修改，移除零宽字符）
     * @return 提取的会话ID，如果未找到返回空字符串
     */
    static std::string extractAndRemoveSessionIdFromText(std::string& text);
    
    /**
     * @brief 将会话ID嵌入到文本末尾
     *
     * @param text 原始文本
     * @param sessionId 要嵌入的会话ID
     * @return 带有嵌入会话ID的文本
     */
    static std::string embedSessionIdInText(const std::string& text, const std::string& sessionId);
    
    // ========== 会话转移两阶段方法（ZeroWidth/Hash 模式共用）==========
    /**
     * @brief 阶段1：预生成下一轮的 sessionId
     *
     * 在响应嵌入之前调用，生成新的 sessionId 并存储到 session.state.nextSessionId。
     * 调用方应将 nextSessionId 嵌入到响应中发送给客户端。
     *
     * @param session 会话对象（会设置 nextSessionId 字段）
     * @return 生成的新 sessionId
     */
    std::string prepareNextSessionId(session_st& session);
    
    /**
     * @brief 阶段2：执行会话转移
     *
     * 在响应发送给客户端之后调用，执行实际的会话转移：
     * - 更新 messageContext（添加本轮对话）
     * - 转移 provider 线程上下文
     * - 更新 session_map（添加新会话，删除旧会话）
     *
     * @param session 会话对象（需要预先设置 nextSessionId）
     */
    void commitSessionTransfer(session_st& session);
    
private:
    // ========== 图片解析辅助方法（ API 和 响应 API 共用）==========
    // 从 数组中提取图片信息
    static void extractImagesFromContent(const Json::Value& content, std::vector<ImageInfo>& images);
    // 解析单个图片项
    static void parseImageItem(const Json::Value& item, std::vector<ImageInfo>& images);
    // 将 转换为字符串，同时提取图片
    static std::string getContentAsString(const Json::Value& content, std::vector<ImageInfo>& images);
    
    // ========== 会话创建/更新辅助方法（消除重复代码）==========
    /**
     * @brief 从请求数据更新现有会话
     *
     * 将 session 中的 requestMessage、requestImages 更新到 session_map 中的目标会话，
     * 并将 session_map 中的完整会话赋值回 session。
     *
     * @param sessionId 目标会话ID
     * @param session 请求会话对象（会被修改为 session_map 中的会话）
     */
    void updateExistingSessionFromRequest(const std::string& sessionId, session_st& session);
    
    /**
     * @brief 初始化新会话并添加到 session_map
     *
     * 设置会话的各种ID字段，并将会话添加到 session_map。
     *
     * @param sessionId 新会话ID
     * @param session 会话对象（会被修改）
     */
    void initializeNewSession(const std::string& sessionId, session_st& session);
};
#endif  
