#ifndef SESSION_H
#define SESSION_H
#include <string>
#include <map>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
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

// session_st / SessionTrackingMode / ApiType 已迁至 domain/model/SessionData.h
#include <application/generation/contracts/GenerationSession.h>
#include <platform/ThreadJoin.h>
#include <domain/port/IProviderRegistry.h>
#include <domain/port/IResponseIndex.h>
#include <domain/port/ISessionPersistence.h>
class chatSession
{
  private:
    std::mutex mutex_;
    std::unordered_map<std::string, session_st> session_map;
    std::unordered_map<std::string, std::string> context_map;//上下文会话id与会话id的映射
    SessionTrackingMode trackingMode_ = SessionTrackingMode::Hash;  // 默认使用Hash模式
    /// 会话持久化开关：由 main.cc 在 ensureTables 成功后开启。
    /// 关闭时所有写穿/懒加载静默跳过，行为与纯内存模式完全一致。
    std::atomic<bool> persistenceEnabled_{false};
    /// 以下四项均可由 config 的 session_persistence 段覆盖（配置以小时为单位，main.cc 换算为秒）；非法值(<=0)保留默认。
    std::atomic<int>  sessionExpireSeconds_{SESSION_EXPIRE_TIME};
    std::atomic<int>  cleanupIntervalSeconds_{SESSION_CLEANUP_INTERVAL};
    std::atomic<int>  dbRetentionSeconds_{SESSION_EXPIRE_TIME};
    std::atomic<bool> storeSessionPayload_{true};
    /// N4: 过期清理线程的停机三件套。
    /// 这两个成员早先就已声明但从未被使用（线程仍走 detach），属于半成品；
    /// 现在真正接上：stopClearExpiredLoop_ 是唯一退出条件，clearExpiredWakeCv_
    /// 让停机不必等满一个 cleanupIntervalSeconds_（默认 1 小时）的睡眠周期。
    std::atomic<bool> stopClearExpiredLoop_{false};
    std::thread clearExpiredThread_;
    /// 每次 startClearExpiredSession() 重建：ThreadCompletion 是一次性信号
    /// （signal 后永久为真）。复用同一实例会让第二次限时停机看到上一条线程
    /// 留下的「已完成」而立刻去 join 一条仍在运行的新线程 —— 限时汇合静默
    /// 退化成无限阻塞。
    platform::ThreadCompletionPtr clearExpiredDone_;
    /// 两个 stop 重载的共同实现。deadline 为 nullptr 即无限等待。
    bool stopClearExpiredWithin(const std::chrono::steady_clock::time_point* deadline);
    std::mutex clearExpiredWakeMutex_;
    std::condition_variable clearExpiredWakeCv_;
    IProviderRegistry* providerRegistry_ = nullptr;
    IResponseIndex* responseIndex_ = nullptr;
    ISessionPersistence* persistence_ = nullptr;
public:
    chatSession();
    ~chatSession();
    chatSession(const chatSession&) = delete;
    chatSession& operator=(const chatSession&) = delete;

    // ========== 会话追踪模式相关方法 ==========
    /**
     * @brief 设置会话追踪模式
     * @param mode 追踪模式
     */
    // ========== 会话持久化（写穿 + 懒加载回填）==========
    /// 开启/关闭持久化。DB 不可用时保持 false，绝不影响请求链路。
    void setPersistenceEnabled(bool enabled) { persistenceEnabled_.store(enabled); }
    bool isPersistenceEnabled() const { return persistenceEnabled_.load(); }

    // ---- 可配置项：内存 TTL / 内存清理间隔 / DB 保留期 / payload 落库开关 ----
    void setSessionExpireSeconds(int v)   { if (v > 0) sessionExpireSeconds_.store(v); }
    int  getSessionExpireSeconds() const  { return sessionExpireSeconds_.load(); }
    void setCleanupIntervalSeconds(int v) { if (v > 0) cleanupIntervalSeconds_.store(v); }
    int  getCleanupIntervalSeconds() const{ return cleanupIntervalSeconds_.load(); }
    void setDbRetentionSeconds(int v)     { if (v > 0) dbRetentionSeconds_.store(v); }
    int  getDbRetentionSeconds() const    { return dbRetentionSeconds_.load(); }
    /// 关闭后不再写 chat_session_state 快照；response_index 映射不受影响。
    void setStoreSessionPayload(bool e)   { storeSessionPayload_.store(e); }
    bool isStoreSessionPayloadEnabled() const { return storeSessionPayload_.load(); }

    /// 将指定会话异步写穿到 chat_session_state（调用方不得持有 mutex_）。
    void persistSession(const std::string& sessionId);

    /// 懒加载：内存未命中时尝试从 DB 回填 session_map。命中返回 true。
    /// 注意：调用方不得持有 mutex_（内部自行加锁写回）。
    bool hydrateSessionFromDb(const std::string& sessionId);

    /// 懒加载：Hash 模式下按 context_key 从 DB 反查并回填 session_map + context_map。
    bool hydrateSessionByContextKey(const std::string& contextKey, std::string& outSessionId);

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
    void setProviderRegistry(IProviderRegistry* registry) { providerRegistry_ = registry; }
    void setResponseIndex(IResponseIndex* index) { responseIndex_ = index; }
    void setPersistence(ISessionPersistence* persistence) { persistence_ = persistence; }
    
    // ========== 基础会话操作方法 ==========
    void addSession(const std::string &ConversationId,session_st &session);
    void delSession(const std::string &ConversationId);
    void getSession(const std::string &ConversationId, session_st &session);
    void updateSession(const std::string &ConversationId,session_st &session);
    void clearExpiredSession();
    void startClearExpiredSession();
    /// N4: 停止过期清理线程并 join。幂等；未启动时直接返回。
    /// 必须在进程退出前调用，否则线程会在 static 析构期间被强行截断，
    /// 可能正持有 mutex_ 或 DB 连接。
    /// 停止清理线程并【无上限】等待其退出。语义与 P4-W1 起完全一致。
    /// 亦是收割上一轮限时停机超预算后残留线程的路径。
    void stopClearExpiredSession();

    /**
     * @brief 限时停止清理线程（P4-W3 / D6）。
     *
     * @return true  线程已在 deadline 前退出并被 join；
     *         false 预算耗尽，线程仍在运行且**未 detach**，仍由本对象持有。
     *
     * 为何会真的超预算：清理循环的睡眠可被 notify 立即打断，但循环体内
     * deleteSessionsOlderThan() 是同步 DB 调用，不可中断。停机信号若在它
     * 执行期间到达，线程要等该调用返回才会检查退出标志。
     *
     * 超预算时刻意不 detach、不清理成员：线程仍在访问本对象的 session_map，
     * 放手会让它在对象销毁后继续写已析构的内存。调用方可稍后用无参
     * stopClearExpiredSession() 重新收割。
     */
    bool stopClearExpiredSession(std::chrono::steady_clock::time_point deadline);

    /// 是否仍有未被 join 的清理线程（限时停机超预算后的残局标志）。
    /// 线程收割干净后转为 false 且不再回真。
    bool hasPendingCleaner() const { return clearExpiredThread_.joinable(); }
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
    std::string getContentAsString(const Json::Value& content, std::vector<ImageInfo>& images);
    
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
