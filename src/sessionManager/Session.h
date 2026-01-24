#ifndef SESSION_H
#define SESSION_H
#include <string>
#include <map>
#include <unordered_map>
#include <mutex>
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

// 前向声明 drogon 类型，避免在头文件中直接 include drogon
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
    Hash,       // 基于消息内容哈希的会话追踪（默认，向后兼容）
    ZeroWidth   // 基于零宽字符嵌入的会话追踪
};

static const int SESSION_EXPIRE_TIME = 86400; //24小时，单位秒数,会话过期时间
//static const int SESSION_MAX_MESSAGES = 4; //上下文会话最大消息条数,一轮两条
// ImageInfo 定义在 GenerationRequest.h 中
#include "GenerationRequest.h"

struct session_st
{
  // ========== 通用字段（Chat API 和 Response API 共用）==========
  std::string selectapi="";             // API 提供者名称 (如 "chaynsapi")
  std::string selectmodel="";           // 模型名称 (如 "GPT-4o")
  std::string systemprompt="";          // 系统提示词 (Chat: system role, Response: instructions)
  std::string requestmessage="";        // 当前用户输入的文本内容
  std::vector<ImageInfo> requestImages; // 当前请求中的图片列表
  Json::Value responsemessage;          // AI 响应内容 (消息文本)
  Json::Value api_response_data;        // 完整的 API 响应数据 (Chat/Response 格式)
  Json::Value client_info;              // 客户端信息 (user-agent, authorization 等)
  Json::Value message_context=Json::Value(Json::arrayValue);  // 对话上下文消息数组
  time_t created_time=0;                // 会话创建时间
  time_t last_active_time=0;            // 最后活跃时间
  
  // ========== Chat API 专用字段 ==========
  std::string preConversationId="";     // 上一轮对话 ID (用于会话迁移)
  std::string curConversationId="";     // 当前对话 ID (session_map 的键)
  std::string contextConversationId=""; // 上下文映射 ID (context_map 使用)
  std::string apiChatinfoConversationId=""; // API 聊天信息 ID
  int contextlength=0;                  // 上下文消息数量
  bool contextIsFull=false;             // 上下文是否已满
  
  // ========== Response API 专用字段 ==========
  std::string response_id="";           // Response API 唯一标识 (resp_xxx 格式)
  bool is_response_api=false;           // 标记此会话是否由 Response API 创建
  bool has_previous_response_id=false;  // 标记客户端是否携带了 previous_response_id
  
  // ========== 现有方法 ==========
  void clearMessageContext()
  {
    message_context.clear();
  }
  void addMessageToContext(const Json::Value& message)
  {
    /*
    if(message_context.size() >= SESSION_MAX_MESSAGES)
    {
      message_context.pop_front();
    }
    */
    message_context.append(message);
  }

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
    void coverSessionresponse(session_st& session);
    static std::string generateConversationKey(const Json::Value& keyData);
    static std::string generateSHA256(const std::string& input);
    static Json::Value getClientInfo(const drogon::HttpRequestPtr &req);
    
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
     * - 根据 session.curConversationId（已从previous_response_id解析）查找会话
     * - 如果会话存在则更新并延续上下文
     * - 如果会话不存在则创建新会话
     *
     * @param session 会话对象（会被修改，需要预先设置 curConversationId）
     * @return 处理后的会话引用
     */
    session_st& createOrUpdateSessionByPreviousResponseId(session_st& session);
    
    /**
     * @deprecated 请使用 createOrUpdateSessionHash() 或 createOrUpdateChatSession()
     * 此方法保留用于向后兼容，内部已重定向到新方法
     */
    [[deprecated("Use createOrUpdateSessionHash() or createOrUpdateChatSession() instead")]]
    session_st& createNewSessionOrUpdateSession(session_st& session);
    
    /**
     * @brief [DEPRECATED] 从 HTTP 请求构建 session_st (Chat API)
     *
     * @deprecated 请使用 RequestAdapters::buildGenerationRequestFromChat() + GenerationService::runGuarded() 替代
     *
     * 此方法已弃用，Controller 不应再直接调用此方法来构建 session。
     * 新的调用路径：RequestAdapters -> GenerationRequest -> GenerationService::runGuarded()
     */
    [[deprecated("Use RequestAdapters::buildGenerationRequestFromChat() + GenerationService::runGuarded() instead")]]
    session_st gennerateSessionstByReq(const drogon::HttpRequestPtr &req);
    
    Json::Value generateJsonbySession(const session_st& session,bool contextIsFull);
    
    // ========== 新增方法（Response API 使用）==========
    // 生成唯一的 response_id (resp_xxx 格式)
    static std::string generateResponseId();
    
    // 为 Response API 创建会话（使用 response_id 作为键）
    std::string createResponseSession(session_st& session);
    
    // 通过 response_id 获取会话
    bool getResponseSession(const std::string& response_id, session_st& session);
    
    // 通过 response_id 删除会话
    bool deleteResponseSession(const std::string& response_id);
    
    // 更新 Response API 会话（不删除旧 session，直接更新）
    void updateResponseSession(session_st& session);
    
    /**
     * @brief [DEPRECATED] 从 HTTP 请求构建 session_st (Response API)
     *
     * @deprecated 请使用 RequestAdapters::buildGenerationRequestFromResponses() + GenerationService::runGuarded() 替代
     *
     * 此方法已弃用，Controller 不应再直接调用此方法来构建 session。
     * 新的调用路径：RequestAdapters -> GenerationRequest -> GenerationService::runGuarded()
     */
    [[deprecated("Use RequestAdapters::buildGenerationRequestFromResponses() + GenerationService::runGuarded() instead")]]
    session_st gennerateSessionstByResponseReq(const drogon::HttpRequestPtr &req);
    
    // ========== 零宽字符追踪模式方法 ==========
    /**
     * @brief 生成用于零宽字符模式的唯一会话ID
     * @return 格式为 "zw_timestamp_random" 的会话ID
     */
    static std::string generateZeroWidthSessionId();
    
    /**
     * @brief 在零宽字符模式下创建或更新会话
     * 
     * 与 createNewSessionOrUpdateSession 不同，此方法：
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
    
private:
    // ========== 图片解析辅助方法（Chat API 和 Response API 共用）==========
    // 从 content 数组中提取图片信息
    static void extractImagesFromContent(const Json::Value& content, std::vector<ImageInfo>& images);
    // 解析单个图片项
    static void parseImageItem(const Json::Value& item, std::vector<ImageInfo>& images);
    // 将 content 转换为字符串，同时提取图片
    static std::string getContentAsString(const Json::Value& content, std::vector<ImageInfo>& images);
    
    // ========== Response API 专用辅助方法 ==========
    /**
     * @brief 在 Response API input 数组中查找最后一条 assistant 消息的索引
     * @param input Response API 的 input 数组
     * @return 最后一条 assistant 消息的索引，未找到返回 -1
     */
    static int findLastAssistantIndexInInput(const Json::Value& input);
    
    /**
     * @brief 从 Response API 的 content 中提取纯文本
     * @param content content 字段（可以是字符串或数组）
     * @param stripZeroWidth 是否移除零宽字符
     * @return 提取的文本内容
     */
    static std::string extractTextFromResponseContent(const Json::Value& content, bool stripZeroWidth = false);
    
    /**
     * @brief 从 Response API input 中提取零宽字符编码的会话ID
     * @param input Response API 的 input 数组
     * @param splitIndex 最后一条 assistant 消息的索引
     * @return 提取的会话ID，未找到返回空字符串
     */
    static std::string extractZeroWidthSessionIdFromResponseInput(const Json::Value& input, int splitIndex);
    
    /**
     * @brief 解析 Response API input 数组中的单个消息项
     * @param item input 数组中的单个元素
     * @param index 当前元素在数组中的索引
     * @param splitIndex 最后一条 assistant 消息的索引
     * @param session 会话对象（会被修改）
     * @param isZeroWidthMode 是否启用零宽字符模式
     */
    static void parseResponseInputItem(const Json::Value& item, int index, int splitIndex, 
                                       session_st& session, bool isZeroWidthMode);
    
    /**
     * @brief 处理 previous_response_id 逻辑
     * @param prevId previous_response_id 值
     * @param session 会话对象（会被修改）
     */
    void handlePreviousResponseId(const std::string& prevId, session_st& session);
    
    // ========== 会话创建/更新辅助方法（消除重复代码）==========
    /**
     * @brief 从请求数据更新现有会话
     *
     * 将 session 中的 requestmessage、requestImages 更新到 session_map 中的目标会话，
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