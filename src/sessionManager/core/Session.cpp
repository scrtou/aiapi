#include "sessionManager/core/Session.h"
#include "sessionManager/continuity/ResponseIndex.h"
#include <time.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include <apiManager/ApiManager.h>
#include <apiManager/Apicomn.h>
#include <tools/ZeroWidthEncoder.h>
#include <random>
#include <chrono>
#include "dbManager/session/SessionDbManager.h"
#include "sessionManager/core/SessionCodec.h"
using namespace drogon;
chatSession *chatSession::instance = nullptr;

chatSession::chatSession()
{
}

chatSession::~chatSession()
{
}

// ========== 会话持久化实现（写穿 + 懒加载回填）==========
// 锁约束：以下三个方法内部自行加/解 mutex_，调用方不得在持锁状态下进入，否则死锁。

void chatSession::persistSession(const std::string& sessionId)
{
    if (!persistenceEnabled_.load() || sessionId.empty()) return;
    // payload 落库开关：关闭时跳过快照写穿（懒加载随之自然失效，退化为纯内存）。
    if (!storeSessionPayload_.load()) return;

    SessionDbManager::SessionRow row;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = session_map.find(sessionId);
        if (it == session_map.end()) return;  // 已被删除：不写穿，避免墓碑复活

        const session_st& s = it->second;
        row.sessionId    = sessionId;
        row.apiName      = s.request.api;
        row.apiType      = s.isResponseApi() ? 1 : 0;
        row.contextKey   = s.state.contextConversationId;
        row.payload      = sessioncodec::encodeSession(s);
        row.createdAt    = static_cast<int64_t>(s.state.createdAt);
        row.lastActiveAt = static_cast<int64_t>(s.state.lastActiveAt);
    }
    // 出锁后再投递异步写入，避免 DB 队列抖动拉长热路径持锁时长。
    SessionDbManager::getInstance()->asyncUpsertSession(row);
}

bool chatSession::hydrateSessionFromDb(const std::string& sessionId)
{
    if (!persistenceEnabled_.load() || sessionId.empty()) return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_map.count(sessionId)) return true;  // 已在内存，无需回填
    }

    auto row = SessionDbManager::getInstance()->loadSession(sessionId);
    if (!row.has_value()) return false;

    session_st restored = sessioncodec::decodeSession(row->payload);
    // 以行主键为准修正 conversationId，避免快照与主键不一致导致索引错位。
    restored.state.conversationId = sessionId;
    if (restored.state.createdAt == 0)    restored.state.createdAt = static_cast<time_t>(row->createdAt);
    if (restored.state.lastActiveAt == 0) restored.state.lastActiveAt = static_cast<time_t>(row->lastActiveAt);

    // 过期行不回填：DB 清理线程可能尚未跑到，此处按 TTL 再判一次。
    if (time(nullptr) - restored.state.lastActiveAt > sessionExpireSeconds_.load()) {
        LOG_INFO << "[会话持久化] 命中已过期快照，按新会话处理: " << sessionId;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 双重检查：回填期间可能已有并发请求建好同名会话，内存优先。
        if (session_map.count(sessionId)) return true;
        session_map[sessionId] = restored;
        if (!restored.state.contextConversationId.empty()) {
            context_map[restored.state.contextConversationId] = sessionId;
        }
    }
    LOG_INFO << "[会话持久化] 已从数据库恢复会话: " << sessionId;
    return true;
}

bool chatSession::hydrateSessionByContextKey(const std::string& contextKey, std::string& outSessionId)
{
    if (!persistenceEnabled_.load() || contextKey.empty()) return false;

    auto row = SessionDbManager::getInstance()->loadSessionByContextKey(contextKey);
    if (!row.has_value() || row->sessionId.empty()) return false;

    if (!hydrateSessionFromDb(row->sessionId)) return false;

    outSessionId = row->sessionId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        context_map[contextKey] = outSessionId;
    }
    LOG_INFO << "[会话持久化] 已按上下文键恢复会话映射: " << outSessionId;
    return true;
}

void chatSession::addSession(const std::string &ConversationId,session_st &session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    session_map[ConversationId] = session;
}

void chatSession::delSession(const std::string &ConversationId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    session_map.erase(ConversationId);
}

void chatSession::getSession(const std::string &ConversationId, session_st &session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = session_map.find(ConversationId);
    if (it == session_map.end())
    {
        LOG_WARN << "[会话管理] getSession: 未找到会话ID";
        return;
    }
    session = it->second;
}

void chatSession::updateSession(const std::string &ConversationId,session_st &session)
{
    if(session_map.find(ConversationId) != session_map.end())
    {    std::lock_guard<std::mutex> lock(mutex_);
        session_map[ConversationId] = session;
    }
}

// ========== 会话创建/更新辅助方法（消除重复代码）==========

void chatSession::updateExistingSessionFromRequest(const std::string& sessionId, session_st& session)
{
    // 关键约束说明：
    // 更新已存在会话时，必须把“请求级字段”合并到已存储会话中，
    // 包含模型、系统提示词、工具定义、当前输入、客户端信息等实时参数。
    // 若不合并，这些字段（尤其 tools）会在续聊时丢失，从而导致工具桥接逻辑失效，
    // 最终表现为后续轮次无法识别客户端传入的工具定义。
    auto &stored = session_map[sessionId];

    // 请求级字段处理策略：始终以本次请求的最新值覆盖，确保行为与客户端输入一致。
    stored.request.api = session.request.api;
    stored.request.model = session.request.model;
    if (!session.request.systemPrompt.empty()) {
        stored.request.systemPrompt = session.request.systemPrompt;
    }
    stored.provider.clientInfo = session.provider.clientInfo;
    stored.request.message = session.request.message;
    stored.request.rawMessage = session.request.rawMessage.empty() ? session.request.message : session.request.rawMessage;
    stored.request.images = session.request.images;
    if (!session.request.tools.isNull() && session.request.tools.isArray() && session.request.tools.size() > 0) {
        stored.request.tools = session.request.tools;
        stored.request.toolsRaw =
            (!session.request.toolsRaw.isNull() && session.request.toolsRaw.isArray())
                ? session.request.toolsRaw
                : session.request.tools;
    } else if (!session.request.toolsRaw.isNull() && session.request.toolsRaw.isArray() && session.request.toolsRaw.size() > 0) {
        stored.request.toolsRaw = session.request.toolsRaw;
    }
    if (!session.request.toolChoice.empty()) {
        stored.request.toolChoice = session.request.toolChoice;
    }
    stored.request.parallelToolCalls = session.request.parallelToolCalls;

    // 协议标记需要同步更新（主要用于 Response API 复用本辅助方法时保持状态一致）。
    // 更新 API 类型和相关标记
    stored.state.apiType = session.state.apiType;
    stored.state.hasPreviousResponseId = session.state.hasPreviousResponseId;
    stored.state.requestId = session.state.requestId;
    
    // 关键约束：在 Response API 的续聊请求中，`session.state.conversationId` 属于当前请求临时新值，
    // 而 `stored.state.conversationId` 必须保持稳定，并与 session_map 的键一致。
    // 因此此处绝不能覆盖 stored.state.conversationId，否则会破坏会话索引一致性。

    stored.state.lastActiveAt = time(nullptr);

    // 将合并后的稳定会话状态回写给调用方，后续流程统一使用该结果。
    session = stored;
}

void chatSession::initializeNewSession(const std::string& sessionId, session_st& session)
{
    session.provider.prevProviderKey = sessionId;
    session.state.conversationId = sessionId;
    if (session.state.createdAt == 0) {
        session.state.createdAt = time(nullptr);
    }
    if (session.state.lastActiveAt == 0) {
        session.state.lastActiveAt = time(nullptr);
    }
    addSession(sessionId, session);
}

// ========== 外层包装方法（含模式判断）==========

session_st& chatSession::createOrUpdateChatSession(session_st& session)
{
    LOG_INFO << "[聊天接口] 创建或更新会话";
    
    // 根据配置选择会话追踪模式
    if (isZeroWidthMode()) {
        LOG_INFO << "[Chat API] 使用零宽字符追踪模式";
        return createOrUpdateSessionZeroWidth(session);
    } else {
        LOG_INFO << "[Chat API] 使用Hash追踪模式";
        return createOrUpdateSessionHash(session);
    }
}

session_st& chatSession::createOrUpdateResponseSession(session_st& session)
{
    LOG_INFO << "[Response API] 创建或更新会话";
    
    // 设置 API 类型（在所有分支之前设置）
    session.state.apiType = ApiType::Responses;
    
    // 优先级1：如果客户端携带了 previous_response_id，从历史会话获取上下文
    if (session.state.hasPreviousResponseId && !session.state.conversationId.empty()) {
        LOG_INFO << "[Response API] 检测到 previous_response_id: " << session.state.conversationId;
        
        // 尝试从历史会话获取上下文
        session_st prevSession;
        if (getResponseSession(session.state.conversationId, prevSession)) {
            // 继承历史消息上下文
            session.provider.messageContext = prevSession.provider.messageContext;
            session.provider.prevProviderKey = session.state.conversationId;
            session.state.isContinuation = true;
            LOG_INFO << "[Response API] 从 previous_response_id 继承上下文, 消息数: "
                     << session.provider.messageContext.size();
        } else {
            LOG_WARN << "[Response API] previous_response_id 对应的会话不存在: "
                     << session.state.conversationId;
            session.state.isContinuation = false;
        }
        
        // 响应接口分支：按响应协议生成会话 ID 使用 previous_response_id 模式时，conversationId 应该由 Controller 预先生成
        // 这里不再调用 createOrUpdateSessionByPreviousResponseId，因为会话创建由 Controller 控制
        return session;
    }
    
    // 优先级2：没有 previous_response_id，检查是否从零宽字符提取了会话ID
    // 响应接口分支：按响应协议生成会话 ID 也支持零宽字符追踪模式（用于不支持 previous_response_id 的客户端）
    if (isZeroWidthMode() && !session.state.hasPreviousResponseId && !session.state.conversationId.empty()) {
        LOG_INFO << "[Response API] 使用零宽字符追踪模式, 提取的会话ID: " << session.state.conversationId;
        
        // 尝试从零宽字符提取的会话ID获取上下文
        session_st prevSession;
        if (getResponseSession(session.state.conversationId, prevSession)) {
            // 继承历史消息上下文
            session.provider.messageContext = prevSession.provider.messageContext;
            session.provider.prevProviderKey = session.state.conversationId;
            session.state.isContinuation = true;
            LOG_INFO << "[Response API] 从零宽字符会话继承上下文, 消息数: "
                     << session.provider.messageContext.size();
        } else {
            LOG_INFO << "[Response API] 零宽字符会话不存在，创建新会话";
            session.state.isContinuation = false;
        }
        
        return session;
    }
    
    // 优先级3：Hash模式或新会话
    LOG_INFO << "[Response API] 新会话（无 previous_response_id，无零宽字符）";
    //Hash模型的会话连续性暂时未实现
    session.state.isContinuation = false;
    return session;
}

session_st& chatSession::getOrCreateSession(const std::string& sessionId, session_st& session)
{
    std::string sid = sessionId;
    if (sid.empty()) {
        // 兜底：理论上不应该发生（ContinuityResolver 一定会给出 sessionId）
        sid = generateZeroWidthSessionId();
        LOG_WARN << "[SessionStore] sessionId 为空，已兜底生成: " << sid;
    }

    // Hash 模式存在 context_map 映射：当 hashKey 未命中 session_map 但命中 context_map 时，
    // 需要将“裁剪后的 hashKey”映射回真实 sessionId（保持旧行为）。
    std::string mapped;
    if (!sessionIsExist(sid) && consumeContextMapping(sid, mapped)) {
        sid = mapped;
    }

    // [持久化] 懒加载回填：内存与 context_map 均未命中时，尝试从数据库恢复。
    // 顺序至关重要——必须在 sessionIsExist 决定“新建/续聊”之前完成，
    // 否则重启后的首个续聊请求会被误判为新会话，丢失全部 messageContext。
    // 持久化关闭或 DB 未命中时全部静默返回 false，退化为原有纯内存行为。
    if (persistenceEnabled_.load() && !sessionIsExist(sid)) {
        if (!hydrateSessionFromDb(sid)) {
            std::string dbMapped;
            if (hydrateSessionByContextKey(sid, dbMapped) && !dbMapped.empty()) {
                sid = dbMapped;
                // 与内存路径语义对齐：命中上下文键说明已处于上下文裁剪边界。
                std::lock_guard<std::mutex> lock(mutex_);
                auto sit = session_map.find(sid);
                if (sit != session_map.end()) {
                    sit->second.state.contextIsFull = true;
                }
            }
        }
    }

    session.state.conversationId = sid;  // sessionId：会话主键（session_map 的 key）
    session.provider.prevProviderKey = sid;  // provider thread map 查找 key

    if (sessionIsExist(sid)) {
        updateExistingSessionFromRequest(sid, session);
        // updateExistingSessionFromRequest() 会用存量会话覆盖 session（包含旧的 prevProviderKey/isContinuation 等）。
        // 这里需要把“本次请求”的续聊语义字段重新写回，避免被覆盖。
        session.state.isContinuation = true;
        session.state.conversationId = sid;
        session.provider.prevProviderKey = sid;
    } else {
        session.state.isContinuation = false;
        initializeNewSession(sid, session);
    }

    // [持久化] 写穿：新建与续聊两条分支收敛于此，统一落库一次快照。
    // 此处未持有 mutex_，满足 persistSession 的锁约束；异步投递不阻塞请求链路。
    persistSession(sid);

    return session;
}

// ========== 底层实现方法（独立功能）==========

session_st& chatSession::createOrUpdateSessionByPreviousResponseId(session_st& session)
{
    LOG_INFO << "[Previous Response ID] 基于previous_response_id创建或更新会话";
    
    std::string prevId = session.state.conversationId;
    
    if (!prevId.empty() && sessionIsExist(session)) {
        // 会话存在，更新并延续上下文
        LOG_INFO << "[Previous Response ID] 延续已存在会话: " << prevId;
        // 标记为继续会话，保存旧的 ID 用于线程上下文转移
        session.state.isContinuation = true;
        session.provider.prevProviderKey = prevId;
        updateExistingSessionFromRequest(prevId, session);
    } else {
        // 会话不存在，创建新会话（使用previous_response_id作为会话ID）
        LOG_INFO << "[Previous Response ID] 会话不存在，创建新会话: " << prevId;
        session.state.isContinuation = false;
        if (!prevId.empty()) {
            initializeNewSession(prevId, session);
        } else {
            // 如果prevId为空，生成新的会话ID
            std::string newSessionId = generateResponseId();
            initializeNewSession(newSessionId, session);
        }
    }
    
    return session;
}

session_st& chatSession::createOrUpdateSessionHash(session_st& session)
{
    LOG_INFO << "[Hash模式] 基于消息内容哈希创建或更新会话";
    
    // Hash 模式：基于消息内容计算会话ID
    std::string tempConversationId = generateConversationKey(generateJsonbySession(session, false));
    LOG_INFO << "[Hash模式] 从请求生成会话ID: " << tempConversationId;

    if (sessionIsExist(tempConversationId))
    {
        LOG_INFO << "[Hash模式] 会话已存在, 正在更新";
        // 标记为继续会话，保存旧的 ID 用于线程上下文转移
        session.state.isContinuation = true;
        session.provider.prevProviderKey = tempConversationId;
        updateExistingSessionFromRequest(tempConversationId, session);
    }
    else
    {
        if (context_map.find(tempConversationId) != context_map.end())
        {
            LOG_INFO << "[Hash模式] 在上下文映射中找到会话";
            std::string mappedSessionId = context_map[tempConversationId];
            session_map[mappedSessionId].state.contextIsFull = true;
            // 标记为继续会话，保存旧的 ID 用于线程上下文转移
            session.state.isContinuation = true;
            session.provider.prevProviderKey = mappedSessionId;
            updateExistingSessionFromRequest(mappedSessionId, session);
            context_map.erase(tempConversationId);
        }
        else
        {
            LOG_INFO << "[Hash模式] 未找到会话, 正在创建新会话";
            session.state.isContinuation = false;
            initializeNewSession(tempConversationId, session);
        }
    }
    
    return session;
}

std::string chatSession::generateConversationKey(
        const Json::Value& keyData
    )
{   // 生成哈希
    // 使用 StyledWriter 保证 JSON 键的顺序，从而确保哈希值的一致性。
    // FastWriter 或默认的 StreamWriterBuilder 不保证顺序。
    Json::StyledWriter writer;
    std::string output = writer.write(keyData);
    return generateSHA256(output);
}

// ========== 会话转移两阶段方法实现 ==========

std::string chatSession::prepareNextSessionId(session_st& session)
{
    // 根据追踪模式生成下一轮的 sessionId
    if (isZeroWidthMode()) {
        session.state.nextSessionId = generateZeroWidthSessionId();
        LOG_INFO << "[ZeroWidth] 预生成下一轮 sessionId: " << session.state.nextSessionId
                 << " (当前: " << session.state.conversationId << ")";
    } else {
        // Hash 模式：基于当前上下文生成新的 sessionId
        // 注意：此时 messageContext 还未包含本轮对话，需要先临时添加
        Json::Value tempContext = session.provider.messageContext;
        
        // 临时添加本轮对话用于计算 hash
        Json::Value userMsg;
        userMsg["role"] = "user";
        userMsg["content"] = session.request.rawMessage.empty() ? session.request.message : session.request.rawMessage;
        tempContext.append(userMsg);
        
        Json::Value assistantMsg;
        assistantMsg["role"] = "assistant";
        assistantMsg["content"] = session.response.message["message"].asString();
        tempContext.append(assistantMsg);
        
        // 构建用于 hash 的 JSON
        Json::Value keyData(Json::objectValue);
        keyData["messages"] = tempContext;
        keyData["clientInfo"] = session.provider.clientInfo;
        keyData["model"] = session.request.model;
        
        session.state.nextSessionId = generateConversationKey(keyData);
        LOG_INFO << "[Hash] 预生成下一轮 sessionId: " << session.state.nextSessionId
                 << " (当前: " << session.state.conversationId << ")";
    }
    
    return session.state.nextSessionId;
}

void chatSession::commitSessionTransfer(session_st& session)
{
    // 1. 更新 messageContext（添加本轮对话）
    Json::Value userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = session.request.rawMessage.empty() ? session.request.message : session.request.rawMessage;
    session.addMessageToContext(userMsg);
    
    Json::Value assistantMsg;
    assistantMsg["role"] = "assistant";
    assistantMsg["content"] = session.response.message["message"].asString();
    session.addMessageToContext(assistantMsg);
    
    session.state.lastActiveAt = time(nullptr);
    
    // 2. 清理临时字段
    session.request.message.clear();
    session.request.rawMessage.clear();
    session.response.message.clear();
    session.provider.toolBridgeTrigger.clear();
    session.provider.toolBridgeFormat = toolcall::BridgeWireFormat::Unset;
    session.provider.toolBridgeAllowFormatFallback = false;
    
    // 3. 执行会话转移
    std::string oldSessionId = session.state.conversationId;
    std::string newSessionId = session.state.nextSessionId;
    
    if (newSessionId.empty()) {
        LOG_ERROR << "[SessionTransfer] nextSessionId 为空，拒绝执行会话转移（请先调用 prepareNextSessionId）";
        return;
    }

    // Codex 使用客户端 thread-id 作为稳定会话键，不应每轮迁移到 Hash/ZeroWidth ID。
    if (newSessionId == oldSessionId) {
        session.provider.prevProviderKey = oldSessionId;
        session.state.nextSessionId.clear();
        updateSession(oldSessionId, session);
        // [持久化] 原位提交路径：此刻快照才含本轮 user+assistant 消息，必须落库；
        // 否则重启后恢复的上下文永远缺最后一轮对话。
        persistSession(oldSessionId);
        LOG_INFO << "[SessionTransfer] 已原位提交稳定会话: " << oldSessionId;
        return;
    }
    
    LOG_INFO << "[SessionTransfer] 执行会话转移: " << oldSessionId << " -> " << newSessionId;
    
    // 4. 更新 session 字段
    session.provider.prevProviderKey = oldSessionId;
    session.state.conversationId = newSessionId;
    session.state.nextSessionId.clear();  // 清理已使用的 nextSessionId
    
    // 5. 转移 provider 线程上下文（如 chaynsapi 的 threadId 映射）
    if (!session.request.api.empty()) {
        auto api = ApiManager::getInstance().getApiByApiName(session.request.api);
        if (api) {
            api->transferThreadContext(oldSessionId, newSessionId);
        }
    }
    
    // 6. 更新 session_map（添加新会话，删除旧会话）
    addSession(newSessionId, session);

    // [Fix] Responses 会话连续性：避免竞态窗口
    // 在删除 oldSessionId 前，确保 responseId 已重新绑定到 newSessionId。
    // 否则可能出现 ResponseIndex 仍指向 oldSessionId，但 oldSessionId 已被删除，导致 previous_response_id 续接断链。
    if (session.state.apiType == ApiType::Responses && !session.response.responseId.empty()) {
        ResponseIndex::instance().bind(session.response.responseId, newSessionId);
        LOG_INFO << "[会话迁移][响应模式] 提前重绑响应 ID 到会话 ID: "
                  << session.response.responseId << " -> " << newSessionId;
    }

    delSession(oldSessionId);

    // [持久化] 会话 ID 轮转：先写新键、再删旧行。
    // 顺序不可颠倒——若先删旧行再写新行，中间崩溃会导致该会话在 DB 中彻底消失；
    // 反之最坏只是短暂多出一行旧快照，会被 TTL 清理回收。
    persistSession(newSessionId);
    if (persistenceEnabled_.load() && !oldSessionId.empty() && oldSessionId != newSessionId) {
        SessionDbManager::getInstance()->asyncDeleteSessions({oldSessionId});
    }
    
    // 7. Hash 模式特有：更新 context_map
    if (!isZeroWidthMode() && !session.state.contextIsFull) {
        LOG_INFO << "[Hash] 上下文未满，更新 contextConversationId";
        session.state.contextLength = session.provider.messageContext.size() - 2;
        std::string tempConversationId = generateConversationKey(generateJsonbySession(session, true));
        context_map.erase(session.state.contextConversationId);
        session.state.contextConversationId = tempConversationId;
        context_map[tempConversationId] = session.state.conversationId;
        updateSession(session.state.conversationId, session);
        // [持久化] contextConversationId 已变更，必须重写该行的 context_key，
        // 否则 DB 中仍是上一轮的旧键，下次按上下文键懒加载将永远落空。
        persistSession(session.state.conversationId);
    }
    
    LOG_INFO << "[SessionTransfer] 会话转移完成, 新 sessionId: " << newSessionId;
}

void chatSession::coverSessionresponse(session_st& session)
{
    if (session.state.nextSessionId.empty()) {
        LOG_ERROR << "[coverSessionresponse] nextSessionId 为空，拒绝执行（请先调用 prepareNextSessionId）";
        return;
    }
    commitSessionTransfer(session);
}

std::string chatSession::generateSHA256(const std::string& input) {
    //计算sha256的耗时
    auto start_time = std::chrono::high_resolution_clock::now();
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    // 使用新的 EVP 接口
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hashLen;
        // 创建 EVP_MD_CTX 上下文
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) {
            throw std::runtime_error("Failed to create EVP_MD_CTX");
        }

        try {
            // 初始化上下文
            if (1 != EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr)) {
                throw std::runtime_error("Failed to initialize digest");
            }

            // 更新数据
            if (1 != EVP_DigestUpdate(ctx, input.c_str(), input.length())) {
                throw std::runtime_error("Failed to update digest");
            }

            // 完成哈希计算
            if (1 != EVP_DigestFinal_ex(ctx, hash, &hashLen)) {
                throw std::runtime_error("Failed to finalize digest");
            }

            // 转换为十六进制字符串
            std::stringstream ss;
            for (unsigned int i = 0; i < hashLen; i++) {
                ss << std::hex << std::setw(2) << std::setfill('0') 
                   << static_cast<int>(hash[i]);
            }

            // 清理上下文
            EVP_MD_CTX_free(ctx);
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
            LOG_INFO << "sha256计算耗时: " << duration.count() << " 微秒";
            return ss.str();
        }
        catch (...) {
            // 确保在发生异常时也能清理上下文
            EVP_MD_CTX_free(ctx);
            throw;
        }
#else
    // 使用旧的接口
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256_CTX sha256;
        SHA256_Init(&sha256);
        SHA256_Update(&sha256, input.c_str(), input.length());
        SHA256_Final(hash, &sha256);

        std::stringstream ss;
        for(int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        }
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
        LOG_INFO << "sha256计算耗时: " << duration.count() << " 微秒";  
        return ss.str();
#endif
 }
void chatSession::clearExpiredSession()
{
    // 存储过期会话信息: (sessionId, apiName, isResponseApi)
    std::vector<std::tuple<std::string, std::string, bool>> expired;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        int chatCount = 0, responseCount = 0;
        for (const auto& pair : session_map) {
            if (pair.second.isResponseApi()) responseCount++;
            else chatCount++;
        }
        LOG_INFO << "开始清除过期会话，当前会话数量:" << session_map.size()
                 << " （聊天会话数: " << chatCount << "，响应会话数: " << responseCount << "）";
        time_t now = time(nullptr);

        for (auto it = session_map.begin(); it != session_map.end();)
        {
            if (now - it->second.state.lastActiveAt > sessionExpireSeconds_.load())
            {
                const std::string sessionId = it->first;
                const std::string apiName = it->second.request.api;
                const bool isRespApi = it->second.isResponseApi();
                expired.emplace_back(sessionId, apiName, isRespApi);

                it = session_map.erase(it);

                // 聊天接口分支：按聊天协议生成会话 ID 会话：需要同步清理 context_map 映射，避免残留脏引用
                // 响应接口分支：按响应协议生成会话 ID 会话：不依赖 context_map，因此无需额外清理
                if (!isRespApi) {
                    for (auto ctxIt = context_map.begin(); ctxIt != context_map.end();)
                    {
                        if (ctxIt->second == sessionId)
                            ctxIt = context_map.erase(ctxIt);
                        else
                            ++ctxIt;
                    }
                }
            }
            else
            {
                ++it;
            }
        }

        chatCount = 0; responseCount = 0;
        for (const auto& pair : session_map) {
            if (pair.second.isResponseApi()) responseCount++;
            else chatCount++;
        }
        LOG_INFO << "清除过期会话完成，剩余会话数量:" << session_map.size()
                 << " （聊天会话数: " << chatCount << "，响应会话数: " << responseCount << "）";
    } // 解锁后再清 provider，避免锁顺序/死锁风险

    // [持久化] 会话已从内存过期淘汰，必须同步删除 DB 行。
    // 否则下次请求会通过懒加载把已过期会话“复活”，TTL 形同虚设、表也会无界增长。
    // 放在解锁之后：DB 投递不在 mutex_ 临界区内，避免拉长持锁时长。
    if (persistenceEnabled_.load() && !expired.empty()) {
        std::vector<std::string> expiredIds;
        expiredIds.reserve(expired.size());
        for (const auto& item : expired) expiredIds.push_back(std::get<0>(item));
        SessionDbManager::getInstance()->asyncDeleteSessions(expiredIds);
        LOG_INFO << "[会话持久化] 已投递过期会话删除，数量: " << expiredIds.size();
    }

    // 清理上游 API Provider 资源，防止会话删除后仍占用上下文映射
    for (const auto& item : expired)
    {
        const std::string& sessionId = std::get<0>(item);
        const std::string& apiName = std::get<1>(item);
        const bool isRespApi = std::get<2>(item);
        
        if (apiName.empty())
            continue;

        auto api = ApiManager::getInstance().getApiByApiName(apiName);
        if (api)
        {
            api->eraseChatinfoMap(sessionId);
        }
        
        if (isRespApi) {
            LOG_INFO << "[响应接口] 清理过期会话: " << sessionId;
        }
    }
}
void chatSession::startClearExpiredSession()
{
    // 轮询间隔必须显著小于过期阈值：若两者相同（原实现均为 SESSION_EXPIRE_TIME），
    // 一个刚过期的会话最坏要等满一个完整周期才被回收，实际生命周期漂移到 24~48h。
    // 此处按 SESSION_CLEANUP_INTERVAL（默认 1 小时）轮询，使过期判定精度收敛到 1 小时内。
    //
    // N4: 由裸 detach 改为持有 std::thread 成员 + 条件变量可中断睡眠。
    // 用 wait_for(谓词) 而非 sleep_for 的原因：停机时 notify 立即唤醒，
    // 停机延迟从「最坏一个完整清理周期（默认 1 小时）」降到毫秒级。
    if (clearExpiredThread_.joinable()) {
        LOG_WARN << "[会话清理] 清理线程已在运行，忽略重复启动";
        return;
    }
    stopClearExpiredLoop_.store(false);

    clearExpiredThread_ = std::thread([this]() {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(clearExpiredWakeMutex_);
                clearExpiredWakeCv_.wait_for(
                    lock,
                    std::chrono::seconds(cleanupIntervalSeconds_.load()),
                    [this] { return stopClearExpiredLoop_.load(); });
            }
            if (stopClearExpiredLoop_.load()) {
                break;
            }

            clearExpiredSession();

            // B5: 同步清理数据库中早于 TTL 的会话快照，防止 chat_session_state 无限增长。
            // 任何 DB 异常在 SessionDbManager 内部已降级忽略，不影响内存清理。
            auto sessionDb = SessionDbManager::getInstance();
            if (sessionDb && sessionDb->isEnabled()) {
                const int64_t cutoff =
                    static_cast<int64_t>(time(nullptr)) - dbRetentionSeconds_.load();
                const int removed = sessionDb->deleteSessionsOlderThan(cutoff);
                if (removed > 0) {
                    LOG_INFO << "[会话持久化] 已清理过期会话快照 " << removed << " 条";
                }
            }
        }
        LOG_INFO << "[会话清理] 过期清理线程已退出";
    });
}

void chatSession::stopClearExpiredSession()
{
    // 幂等：未启动或已停机时直接返回，重复调用安全（main.cc 显式调用 + 析构兜底）。
    if (!clearExpiredThread_.joinable()) {
        return;
    }
    {
        // 置位必须在持锁状态下完成，否则可能与 wait_for 的谓词检查交错，
        // 造成 notify 早于 wait 而丢失唤醒，停机退化为等满一个周期。
        std::lock_guard<std::mutex> lock(clearExpiredWakeMutex_);
        stopClearExpiredLoop_.store(true);
    }
    clearExpiredWakeCv_.notify_all();
    clearExpiredThread_.join();
}

bool chatSession::sessionIsExist(const std::string &ConversationId)
{
    // 判断会话是否存在：要求会话键命中且模型一致（用于避免串会话）
    auto it=session_map.find(ConversationId);
    if(it!=session_map.end())
    {
            return true;
    }
    return false;
}
bool chatSession::sessionIsExist(session_st &session)
{
    // 判断会话是否存在：要求会话键命中且模型一致（用于避免串会话）
    auto it=session_map.find(session.state.conversationId);
    if(it!=session_map.end())
    {
        if(it->second.request.model==session.request.model&&it->second.request.api==session.request.api)
        {
            return true;
        }
    }
    return false;

}

bool chatSession::consumeContextMapping(const std::string& contextConversationId, std::string& outSessionId)
{
    if (contextConversationId.empty()) return false;

    auto it = context_map.find(contextConversationId);
    if (it == context_map.end()) return false;

    outSessionId = it->second;
    context_map.erase(it);

    // 保持旧行为：标记目标会话 contextIsFull=true
    auto sit = session_map.find(outSessionId);
    if (sit != session_map.end()) {
        sit->second.state.contextIsFull = true;
    }

    return !outSessionId.empty();
}

// ========== 图片解析辅助方法实现 (Chat API 和 Response API 共用) ==========

void chatSession::extractImagesFromContent(const Json::Value& content, std::vector<ImageInfo>& images)
{
    if (!content.isArray()) return;
    for (const auto& item : content) {
        parseImageItem(item, images);
    }
}

void chatSession::parseImageItem(const Json::Value& item, std::vector<ImageInfo>& images)
{
    if (!item.isObject() || !item.isMember("type") || item["type"].asString() != "image_url") {
        return;
    }
    if (!item.isMember("image_url") || !item["image_url"].isObject()) {
        return;
    }
    
    const auto& imageUrl = item["image_url"];
    if (!imageUrl.isMember("url") || !imageUrl["url"].isString()) {
        return;
    }
    
    std::string url = imageUrl["url"].asString();
    ImageInfo imgInfo;
    
    // 检查是否是 base64 编码的图片
    if (url.find("data:") == 0) {
        // 格式: data:image/png;base64,xxxxx
        size_t semicolonPos = url.find(";");
        size_t commaPos = url.find(",");
        if (semicolonPos != std::string::npos && commaPos != std::string::npos) {
            imgInfo.mediaType = url.substr(5, semicolonPos - 5); // 提取 image/png
            imgInfo.base64Data = url.substr(commaPos + 1);       // 提取 base64 数据
        }
    } else {
        // 直接是URL
        imgInfo.uploadedUrl = url;
    }
    
    if (!imgInfo.base64Data.empty() || !imgInfo.uploadedUrl.empty()) {
        images.push_back(imgInfo);
    }
}

std::string chatSession::getContentAsString(const Json::Value& content, std::vector<ImageInfo>& images)
{
    if (content.isString()) {
        std::string text = content.asString();
        // 如果使用零宽字符模式，从文本中移除可能存在的零宽字符编码
        if (getInstance()->isZeroWidthMode()) {
            text = ZeroWidthEncoder::stripZeroWidth(text);
        }
        return text;
    }
    if (content.isArray()) {
        std::string result;
        for (const auto& item : content) {
            if (item.isObject() && item.isMember("type")) {
                std::string itemType = item["type"].asString();
                // 处理文本类型：支持 "text" (Chat API) 和 "input_text" (Response API)
                if ((itemType == "text" || itemType == "input_text") && item.isMember("text") && item["text"].isString()) {
                    std::string textPart = item["text"].asString();
                    // 如果使用零宽字符模式，从文本中移除可能存在的零宽字符编码
                    if (getInstance()->isZeroWidthMode()) {
                        textPart = ZeroWidthEncoder::stripZeroWidth(textPart);
                    }
                    result += textPart;
            
                    // 如果当前片段不以换行符结尾，手动补一个换行
                    if (!textPart.empty() && textPart.back() != '\n') {
                        result += "\n";
                    }
                }
                // 处理图片类型：支持 "image_url" (Chat API)
                else if (itemType == "image_url") {
                    parseImageItem(item, images);
                }
                // 处理图片类型：支持 "input_image" (Response API)
                else if (itemType == "input_image") {
                    ImageInfo imgInfo;
                    // 响应接口分支：按响应协议生成会话 ID 的 input_image 格式可能有多种
                    std::string url;
                    if (item.isMember("image_url") && item["image_url"].isString()) {
                        url = item["image_url"].asString();
                    } else if (item.isMember("url") && item["url"].isString()) {
                        url = item["url"].asString();
                    } else if (item.isMember("file") && item["file"].isObject()) {
                        const auto& fileObj = item["file"];
                        if (fileObj.isMember("url") && fileObj["url"].isString()) {
                            url = fileObj["url"].asString();
                        }
                    }
                    
                    if (!url.empty()) {
                        if (url.find("data:") == 0) {
                            size_t semicolon = url.find(';');
                            size_t comma = url.find(',');
                            if (semicolon != std::string::npos && comma != std::string::npos) {
                                imgInfo.mediaType = url.substr(5, semicolon - 5);
                                imgInfo.base64Data = url.substr(comma + 1);
                            }
                        } else {
                            imgInfo.uploadedUrl = url;
                        }
                        
                        if (!imgInfo.base64Data.empty() || !imgInfo.uploadedUrl.empty()) {
                            images.push_back(imgInfo);
                            LOG_INFO << "[getContentAsString] 提取到图片(input_image), mediaType: " << imgInfo.mediaType;
                            LOG_INFO  << ", hasBase64: " << (!imgInfo.base64Data.empty())
                                     << ", hasUrl: " << (!imgInfo.uploadedUrl.empty());
                        }
                    }
                }
            }
        }
        return result;
    }
    return "";
}

// ========== 会话生成方法 ==========

Json::Value chatSession::generateJsonbySession(const session_st& session,bool contextIsFull)
{
     Json::Value keyData;
     Json::Value messages(Json::arrayValue);
     int startIndex=contextIsFull?(session.provider.messageContext.size()-session.state.contextLength):0;
     for(int i=startIndex;i<session.provider.messageContext.size();i++)
     {
        messages.append(session.provider.messageContext[i]);
     }
     keyData["messages"] = messages;
     keyData["clientInfo"] = session.provider.clientInfo;
     keyData["model"] = session.request.model;
     // Context includes the user's prompt, conversation history, and may
     // include image URLs.  It is used as hash input but must never be copied
     // verbatim to application logs.
     LOG_INFO << "生成 ConversationId 输入摘要: messageCount=" << messages.size()
              << ", clientInfoPresent=" << session.provider.clientInfo.isObject()
              << ", modelPresent=" << !session.request.model.empty();
     return keyData;
}

// ========== Response API 方法实现 ==========

std::string chatSession::generateResponseId()
{
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    return "resp_" + std::to_string(timestamp) + "_" + std::to_string(dis(gen));
}

std::string chatSession::generateCurConversationId(ApiType apiType, SessionTrackingMode mode, bool hasPreviousResponseId)
{
    // 响应接口分支：按响应协议生成会话 ID
    if (apiType == ApiType::Responses) {
        if (hasPreviousResponseId) {
            // 客户端携带了 previous_response_id，生成 resp_xxx 格式
            return generateResponseId();
        } else if (mode == SessionTrackingMode::ZeroWidth) {
            // 零宽字符模式，生成 zw_xxx 格式
            return generateZeroWidthSessionId();
        } else {
            // Hash模式：需要回复后才能确定，返回空字符串表示延迟生成
            return "";
        }
    }
    
    // 聊天接口分支：按聊天协议生成会话 ID
    if (mode == SessionTrackingMode::ZeroWidth) {
        // 零宽字符模式，生成 zw_xxx 格式
        return generateZeroWidthSessionId();
    }
    
    // Hash模式：需要回复后才能确定，返回空字符串表示延迟生成
    return "";
}

std::string chatSession::createResponseSession(session_st& session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 设置 API 类型
    session.state.apiType = ApiType::Responses;
    
    // conversationId 应该已经在调用此方法前通过 generateCurConversationId 生成
    // 如果为空，则生成一个（兜底逻辑）
    if (session.state.conversationId.empty()) {
        session.state.conversationId = generateCurConversationId(
            session.state.apiType,
            trackingMode_,
            session.state.hasPreviousResponseId
        );
        // 如果仍然为空（Hash模式），使用 resp_xxx 格式作为兜底
        if (session.state.conversationId.empty()) {
            session.state.conversationId = generateResponseId();
        }
    }
    
    if (session.state.createdAt == 0) {
        session.state.createdAt = time(nullptr);
    }
    if (session.state.lastActiveAt == 0) {
        session.state.lastActiveAt = time(nullptr);
    }
    
    // 使用 conversationId 作为 session_map 的键
    session_map[session.state.conversationId] = session;
    
    LOG_INFO << "[Response API] 创建会话, conversationId: " << session.state.conversationId;
    return session.state.conversationId;
}

bool chatSession::getResponseSession(const std::string& sessionId, session_st& session)
{
    // 第一跳：内存热层。conversationId 就是 session_map 的键，直接查找。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = session_map.find(sessionId);
        if (it != session_map.end()) {
            session = it->second;
            return true;
        }
    }

    // 第二跳：内存未命中时回查数据库并回填，支撑
    // "内存已淘汰(6h) 但会话快照仍在(24h)" 窗口内的 previous_response_id 续接。
    // 注意：hydrateSessionFromDb 内部自行加锁，调用前必须已释放 mutex_，否则死锁。
    if (hydrateSessionFromDb(sessionId)) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = session_map.find(sessionId);
        if (it != session_map.end()) {
            session = it->second;
            LOG_INFO << "[响应接口] 会话已从数据库回填: " << sessionId;
            return true;
        }
    }

    LOG_WARN << "[响应接口] 获取响应会话失败：未找到会话 ID: " << sessionId;
    return false;
}

bool chatSession::deleteResponseSession(const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 现在 conversationId 就是 session_map 的键，直接查找
    auto it = session_map.find(sessionId);
    
    
    if (it == session_map.end()) {
        LOG_WARN << "[响应接口] 删除响应会话失败：未找到会话 ID: " << sessionId;
        return false;
    }
    
    // 如果有关联的 API，清理其资源
    const std::string& apiName = it->second.request.api;
    const std::string keyToDelete = it->first;
    if (!apiName.empty()) {
        auto api = ApiManager::getInstance().getApiByApiName(apiName);
        if (api) {
            api->eraseChatinfoMap(keyToDelete);
        }
    }
    
    session_map.erase(it);
    LOG_INFO << "[Response API] 删除会话, sessionId: " << sessionId << ", key: " << keyToDelete;
    return true;
}

void chatSession::updateResponseSession(session_st& session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 使用 conversationId 作为键
    if (session.state.conversationId.empty()) {
        LOG_WARN << "[响应接口] 更新响应会话失败：当前会话 ID 为空";
        return;
    }
    
    auto it = session_map.find(session.state.conversationId);
    if (it == session_map.end()) {
        LOG_WARN << "[响应接口] 更新响应会话失败：未找到当前会话 ID: " << session.state.conversationId;
        return;
    }
    
    // 响应接口分支：按响应协议生成会话 ID 不删除旧 session，直接更新
    session.state.lastActiveAt = time(nullptr);
    
    // 更新上下文
    Json::Value userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = session.request.rawMessage.empty() ? session.request.message : session.request.rawMessage;
    session.addMessageToContext(userMsg);
    
    Json::Value assistantMsg;
    assistantMsg["role"] = "assistant";
    assistantMsg["content"] = session.response.message["message"].asString();
    session.addMessageToContext(assistantMsg);

    // 清理临时数据（避免把超大输入/输出持久化到 session_map）
    session.request.message.clear();
    session.request.rawMessage.clear();
    session.response.message.clear();
    session.provider.toolBridgeTrigger.clear();
    session.provider.toolBridgeFormat = toolcall::BridgeWireFormat::Unset;
    session.provider.toolBridgeAllowFormatFallback = false;

    // Provider 线程上下文转移（在发送响应给客户端后进行）
    // 只有当 isContinuation 为 true 且 prevProviderKey 与 conversationId 不同时才需要转移
    if (session.state.isContinuation && !session.request.api.empty() &&
        !session.provider.prevProviderKey.empty() && session.provider.prevProviderKey != session.state.conversationId) {
        auto api = ApiManager::getInstance().getApiByApiName(session.request.api);
        if (api) {
            LOG_INFO << "[Response API] 转移线程上下文: " << session.provider.prevProviderKey << " -> " << session.state.conversationId;
            api->transferThreadContext(session.provider.prevProviderKey, session.state.conversationId);
        }
    }

    // 使用 conversationId 作为键更新
    session_map[session.state.conversationId] = session;
    
    LOG_INFO << "[Response API] 更新会话, conversationId: " << session.state.conversationId;
}

bool chatSession::updateResponseApiData(const std::string& sessionId, const Json::Value& apiData)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (sessionId.empty()) {
        LOG_WARN << "[响应接口] 更新响应 API 数据失败：会话 ID 为空";
        return false;
    }

    // 现在 conversationId 就是 session_map 的键，直接查找
    auto it = session_map.find(sessionId);
    if (it != session_map.end()) {
        it->second.response.apiData = apiData;
        it->second.state.lastActiveAt = time(nullptr);
        return true;
    }
    
    LOG_WARN << "[响应接口] 更新响应 API 数据失败：未找到会话 ID: " << sessionId
             << ", creating minimal session";

    session_st s;
    s.state.conversationId = sessionId;
    s.state.apiType = ApiType::Responses;
    s.state.createdAt = time(nullptr);
    s.state.lastActiveAt = time(nullptr);
    s.response.apiData = apiData;
    session_map[sessionId] = std::move(s);
    return false;
}

// ========== 零宽字符追踪模式方法实现 ==========

std::string chatSession::generateZeroWidthSessionId()
{
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(1000, 9999);
    return "zw_" + std::to_string(timestamp) + "_" + std::to_string(dis(gen));
}

session_st& chatSession::createOrUpdateSessionZeroWidth(session_st& session)
{
    LOG_INFO << "[ZeroWidth] 开始处理零宽字符模式会话";
    
    // 会话ID 由请求适配阶段提前写入 conversationId（若存在）
    // 这里仅检查 conversationId 是否已设置
    std::string extractedSessionId = session.state.conversationId;
    
    if (!extractedSessionId.empty() && sessionIsExist(session))
    {
        // 找到了有效的会话ID，更新现有会话
        LOG_INFO << "[ZeroWidth] 找到旧会话: " << extractedSessionId;
        updateExistingSessionFromRequest(extractedSessionId, session);
        
        // 生成新的会话 ID（像 Response 接口一样，每次都变化）
        std::string newSessionId = generateZeroWidthSessionId();
        LOG_INFO << "[ZeroWidth] 生成新的会话ID: " << newSessionId << " (旧: " << extractedSessionId << ")";
        
        // 迁移会话：从旧 ID 迁移到新 ID
        session_map[newSessionId] = session;
        session_map[newSessionId].state.conversationId = newSessionId;
        session_map[newSessionId].provider.prevProviderKey = extractedSessionId;
        delSession(extractedSessionId);
        
        // 更新 session 引用
        session = session_map[newSessionId];
        
        // 标记为继续会话，需要转移线程上下文
        session.state.isContinuation = true;
        session.provider.prevProviderKey = extractedSessionId;
    }
    else
    {
        // 没有找到有效的会话ID，创建新会话
        LOG_INFO << "[ZeroWidth] 创建新会话";
        std::string newSessionId = generateZeroWidthSessionId();
        initializeNewSession(newSessionId, session);
        session.state.isContinuation = false;
    }
    
    return session;
}

std::string chatSession::extractSessionIdFromText(const std::string& text)
{
    auto result = ZeroWidthEncoder::decode(text);
    return result.value_or("");
}

std::string chatSession::extractAndRemoveSessionIdFromText(std::string& text)
{
    auto result = ZeroWidthEncoder::extractAndRemove(text);
    return result.value_or("");
}

std::string chatSession::embedSessionIdInText(const std::string& text, const std::string& sessionId)
{
    if (sessionId.empty()) {
        return text;
    }
    return ZeroWidthEncoder::appendEncoded(text, sessionId);
}
