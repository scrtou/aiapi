#include "RequestAdapters.h"
#include <tools/ZeroWidthEncoder.h>
#include <drogon/drogon.h>

using namespace drogon;

GenerationRequest RequestAdapters::buildGenerationRequestFromChat(
    const HttpRequestPtr& req
) {
    LOG_INFO << "[RequestAdapters] 从 Chat API 请求构建 GenerationRequest";
    
    GenerationRequest genReq;
    genReq.protocol = OutputProtocol::ChatCompletions;
    
    auto jsonPtr = req->getJsonObject();
    if (!jsonPtr) {
        LOG_WARN << "[RequestAdapters] Chat API 请求体无效";
        return genReq;
    }
    
    const Json::Value& reqBody = *jsonPtr;
    
    // 1. 提取基本参数
    genReq.model = reqBody.get("model", "").asString();
    //一些模型映射
    if(genReq.model=="CAI-CL045")
    {
        genReq.model="Claude Opus 4.5";
    }
    genReq.stream = reqBody.get("stream", false).asBool();
    genReq.provider = "chaynsapi";  // 默认 provider
    
    // 1.1 提取 tools 定义
    if (reqBody.isMember("tools") && reqBody["tools"].isArray()) {
        genReq.tools = reqBody["tools"];
        LOG_INFO << "[RequestAdapters] Chat API 请求包含 " << genReq.tools.size() << " 个工具定义";
    }
    
    // 1.2 提取 tool_choice
    if (reqBody.isMember("tool_choice")) {
        if (reqBody["tool_choice"].isString()) {
            genReq.toolChoice = reqBody["tool_choice"].asString();
        } else if (reqBody["tool_choice"].isObject()) {
            //？
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            genReq.toolChoice = Json::writeString(writer, reqBody["tool_choice"]);
        }
    }
    
    // 2. 提取客户端信息
    genReq.clientInfo = extractClientInfo(req);
    
    // 3. 解析 messages 数组
    std::vector<ImageInfo> images;
    std::string extractedSessionId;
    if (reqBody.isMember("messages") && reqBody["messages"].isArray()) {
        parseChatMessages(
            reqBody["messages"],
            genReq.messages,
            genReq.systemPrompt,
            genReq.currentInput,
            images,
            extractedSessionId
        );
        
        // 如果从零宽字符中提取到了会话ID，设置 sessionKey
        if (!extractedSessionId.empty()) {
            genReq.sessionKey = extractedSessionId;
            LOG_INFO << "[RequestAdapters] Chat API 从零宽字符提取到会话ID: " << extractedSessionId;
        }
    }
    
    // 将提取的图片传递给 GenerationRequest
    genReq.images = images;
    
    LOG_INFO << "[RequestAdapters] Chat API 请求解析完成, model: " << genReq.model
             << ", messages: " << genReq.messages.size()
             << ", currentInput length: " << genReq.currentInput.length()
             << ", images: " << images.size();
    
    return genReq;
}

GenerationRequest RequestAdapters::buildGenerationRequestFromResponses(
    const HttpRequestPtr& req
) {
    LOG_INFO << "[RequestAdapters] 从 Responses API 请求构建 GenerationRequest";
    
    GenerationRequest genReq;
    genReq.protocol = OutputProtocol::Responses;
    
    auto jsonPtr = req->getJsonObject();
    if (!jsonPtr) {
        LOG_WARN << "[RequestAdapters] Responses API 请求体无效";
        return genReq;
    }
    
    const Json::Value& reqBody = *jsonPtr;
    
    // 1. 提取基本参数
    genReq.model = reqBody.get("model", "GPT-4o").asString();
    genReq.stream = reqBody.get("stream", false).asBool();
    genReq.systemPrompt = reqBody.get("instructions", "").asString();
    genReq.provider = "chaynsapi";  // 默认 provider
    
    // 1.1 提取 tools 定义
    if (reqBody.isMember("tools") && reqBody["tools"].isArray()) {
        genReq.tools = reqBody["tools"];
        LOG_INFO << "[RequestAdapters] Responses API 请求包含 " << genReq.tools.size() << " 个工具定义";
    }
    
    // 1.2 提取 tool_choice
    if (reqBody.isMember("tool_choice")) {
        if (reqBody["tool_choice"].isString()) {
            genReq.toolChoice = reqBody["tool_choice"].asString();
        } else if (reqBody["tool_choice"].isObject()) {
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            genReq.toolChoice = Json::writeString(writer, reqBody["tool_choice"]);
        }
    }
    
    // 2. 提取客户端信息
    genReq.clientInfo = extractClientInfo(req);
    
    // 3. 处理 previous_response_id（用于续聊）
    if (reqBody.isMember("previous_response_id") && 
        !reqBody["previous_response_id"].asString().empty()) {
        genReq.previousKey = reqBody["previous_response_id"].asString();
        LOG_INFO << "[RequestAdapters] 检测到 previous_response_id: " << genReq.previousKey;
    }
    
    // 4. 解析 input 字段
    std::vector<ImageInfo> images;
    std::string extractedSessionId;
    if (reqBody.isMember("input")) {
        parseResponseInput(
            reqBody["input"],
            genReq.messages,
            genReq.currentInput,
            images,
            extractedSessionId
        );
        
        // 如果从零宽字符中提取到了会话ID，设置 sessionKey
        bool isZeroWidthMode = chatSession::getInstance()->isZeroWidthMode();

        if (isZeroWidthMode)
        {
            if (!extractedSessionId.empty()) {
                genReq.sessionKey = extractedSessionId;
                LOG_INFO << "[RequestAdapters] 从零宽字符提取到会话ID: " << extractedSessionId;
            }
        }

    }
    
    // 将提取的图片传递给 GenerationRequest
    genReq.images = images;
    
    LOG_INFO << "[RequestAdapters] Responses API 请求解析完成, model: " << genReq.model
             << ", messages: " << genReq.messages.size()
             << ", currentInput length: " << genReq.currentInput.length()
             << ", images: " << images.size();
    
    return genReq;
}

Json::Value RequestAdapters::extractClientInfo(const HttpRequestPtr& req) {
    Json::Value clientInfo;
    
    // 提取 User-Agent 并识别客户端类型
    std::string userAgent = req->getHeader("user-agent");
    std::string clientType = "";
    
    if (userAgent.find("Kilo-Code") != std::string::npos) {
        clientType = "Kilo-Code";
    } else if (userAgent.find("RooCode") != std::string::npos) {
        clientType = "RooCode";
    }
    
    clientInfo["client_type"] = clientType;
    
    // 提取 Authorization
    std::string auth = req->getHeader("authorization");
    if (auth.empty()) {
        auth = req->getHeader("Authorization");
    }
    
    // 移除 Bearer 前缀
    auto stripBearer = [](std::string& s) {
        const std::string p1 = "Bearer ";
        const std::string p2 = "bearer ";
        if (s.rfind(p1, 0) == 0) s = s.substr(p1.size());
        else if (s.rfind(p2, 0) == 0) s = s.substr(p2.size());
        // 简单 trim
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    };
    stripBearer(auth);
    clientInfo["client_authorization"] = auth;
    
    LOG_INFO << "[RequestAdapters] 识别到客户端类型: " << (clientType.empty() ? "Unknown" : clientType);
    LOG_INFO << "[RequestAdapters] 识别到客户 authorization: " << (auth.empty() ? "empty" : auth);
    
    return clientInfo;
}

void RequestAdapters::parseChatMessages(
    const Json::Value& messages,
    std::vector<Message>& result,
    std::string& systemPrompt,
    std::string& currentInput,
    std::vector<ImageInfo>& images,
    std::string& extractedSessionId
) {
    if (!messages.isArray() || messages.empty()) {
        return;
    }
    
    // 查找最后一个 assistant 消息的索引（用于分割历史和当前输入）
    int splitIndex = -1;
    for (int i = static_cast<int>(messages.size()) - 1; i > 0; i--) {
        if (messages[i]["role"].asString() == "assistant") {
            splitIndex = i;
            break;
        }
    }
    
    // 检查是否使用零宽字符模式
    bool isZeroWidthMode = chatSession::getInstance()->isZeroWidthMode();
    
    // 如果使用零宽字符模式，尝试从最后一个 assistant 消息中提取会话ID
    if (isZeroWidthMode && splitIndex > 0) {
        const Json::Value& lastAssistantContent = messages[splitIndex]["content"];
        std::string lastAssistantText;
        if (lastAssistantContent.isString()) {
            lastAssistantText = lastAssistantContent.asString();
        } else if (lastAssistantContent.isArray()) {
            for (const auto& item : lastAssistantContent) {
                if (item.isObject() && item.isMember("type") &&
                    item["type"].asString() == "text") {
                    lastAssistantText += item.get("text", "").asString();
                }
            }
        }
        // 提取会话ID并通过输出参数返回
        extractedSessionId = chatSession::extractSessionIdFromText(lastAssistantText);
        if (!extractedSessionId.empty()) {
            LOG_INFO << "[RequestAdapters] 从历史 assistant 消息中提取到会话ID: " << extractedSessionId;
        }
    }
    
    // 用于临时存储历史消息中的图片（不加入当前请求图片）
    std::vector<ImageInfo> tempImages;
    
    for (int i = 0; i < static_cast<int>(messages.size()); i++) {
        const Json::Value& msg = messages[i];
        std::string role = msg.get("role", "user").asString();
        
        // 处理 system 消息
        if (role == "system") {
            systemPrompt += extractContentText(msg["content"], tempImages, isZeroWidthMode);
            continue;
        }
        
        // 分割历史消息和当前输入
        if (i <= splitIndex) {
            // 历史消息：添加到 messages 列表
            Message message;
            if (role == "user") {
                message.role = MessageRole::User;
            } else if (role == "assistant") {
                message.role = MessageRole::Assistant;
            } else if (role == "tool") {
                message.role = MessageRole::Tool;
                // 提取 tool_call_id
                if (msg.isMember("tool_call_id")) {
                    message.toolCallId = msg["tool_call_id"].asString();
                }
            } else {
                continue;  // 跳过未知角色
            }
            
            std::string text = extractContentText(msg["content"], tempImages, isZeroWidthMode);
            ContentPart part;
            part.type = ContentPartType::Text;
            part.text = text;
            message.content.push_back(part);
            
            // 提取 assistant 消息中的 tool_calls
            if (role == "assistant" && msg.isMember("tool_calls") && msg["tool_calls"].isArray()) {
                for (const auto& tc : msg["tool_calls"]) {
                    message.toolCalls.push_back(tc);
                }
            }
            
            // 合并连续的相同角色消息（但不合并带 tool_calls 的消息）
            if (!result.empty() && result.back().role == message.role &&
                message.toolCalls.empty() && result.back().toolCalls.empty() &&
                message.role != MessageRole::Tool) {
                result.back().content[0].text += text;
            } else {
                result.push_back(message);
            }
        } else {
            // 当前输入：
            // - 标准 ChatCompletions: 最后由 user 消息作为当前输入
            // - Kilo-Code 等 tool-calling 客户端：可能以 role=tool 发送最新的 tool 结果/feedback
            //   作为本轮模型需要处理的“当前输入”，因此这里也要接收 tool 消息。
            if (role == "user" || role == "tool") {
                currentInput += extractContentText(msg["content"], images, isZeroWidthMode);
            }
        }
    }
}

void RequestAdapters::parseResponseInput(
    const Json::Value& input,
    std::vector<Message>& messages,
    std::string& currentInput,
    std::vector<ImageInfo>& images,
    std::string& extractedSessionId
) {
    // 检查是否使用零宽字符模式
    bool isZeroWidthMode = chatSession::getInstance()->isZeroWidthMode();
    
    if (input.isString()) {
        // 简单字符串输入
        currentInput = input.asString();
        if (isZeroWidthMode) {
            ZeroWidthEncoder::stripZeroWidth(currentInput);
        }
        return;
    }
    
    if (!input.isArray()) {
        return;
    }
    
    // 查找最后一个 assistant 消息的索引
    int splitIndex = -1;
    for (int i = static_cast<int>(input.size()) - 1; i >= 0; i--) {
        const auto& item = input[i];
        if (item.isObject() && item.get("role", "").asString() == "assistant") {
            splitIndex = i;
            break;
        }
    }
    
    // 如果使用零宽字符模式，尝试从最后一个 assistant 消息中提取会话ID
    if (isZeroWidthMode && splitIndex >= 0) {
        const auto& lastAssistantItem = input[splitIndex];
        if (lastAssistantItem.isMember("content")) {
            std::string lastAssistantText;
            const auto& content = lastAssistantItem["content"];
            if (content.isString()) {
                lastAssistantText = content.asString();
            } else if (content.isArray()) {
                for (const auto& c : content) {
                    if (c.isObject()) {
                        std::string ctype = c.get("type", "").asString();
                        if (ctype == "output_text" || ctype == "text" || ctype == "input_text") {
                            lastAssistantText += c.get("text", "").asString();
                        }
                    }
                }
            }
            extractedSessionId = chatSession::extractSessionIdFromText(lastAssistantText);
        }
    }
    
    // 用于临时存储历史消息中的图片
    std::vector<ImageInfo> historyImages;
    
    // 解析 input 数组中的每个元素
    for (int i = 0; i < static_cast<int>(input.size()); i++) {
        const auto& item = input[i];
        
        if (item.isString()) {
            // 简单字符串，作为当前请求的一部分
            std::string text = item.asString();
            if (isZeroWidthMode) {
                ZeroWidthEncoder::stripZeroWidth(text);
            }
            currentInput += text + "\n";
            continue;
        }
        
        if (!item.isObject()) {
            continue;
        }
        
        std::string type = item.get("type", "").asString();
        std::string role = item.get("role", "").asString();
        
        // 处理带 role 的消息（历史对话格式）
        if (!role.empty()) {
            if (i <= splitIndex) {
                // 历史消息：添加到 messages 列表
                Message message;
                if (role == "user") {
                    message.role = MessageRole::User;
                } else if (role == "assistant") {
                    message.role = MessageRole::Assistant;
                } else {
                    continue;
                }
                
                std::string msgContent;
                if (item.isMember("content")) {
                    msgContent = extractContentText(item["content"], historyImages, isZeroWidthMode);
                }
                
                ContentPart part;
                part.type = ContentPartType::Text;
                part.text = msgContent;
                message.content.push_back(part);
                messages.push_back(message);
            } else {
                // 当前请求：只处理 user 消息
                if (role == "user") {
                    if (item.isMember("content")) {
                        currentInput += extractContentText(item["content"], images, isZeroWidthMode);
                    }
                }
            }
            continue;
        }
        
        // 处理简单的 input_text 类型（没有 role）
        if (type == "input_text" || type == "text") {
            std::string textContent = item.get("text", "").asString();
            if (isZeroWidthMode) {
                ZeroWidthEncoder::stripZeroWidth(textContent);
            }
            currentInput += textContent;
            continue;
        }
        
        // 处理图片输入 (input_image 类型)
        if (type == "input_image") {
            std::string url;
            if (item.isMember("image_url")) {
                url = item["image_url"].asString();
            } else if (item.isMember("url")) {
                url = item["url"].asString();
            } else if (item.isMember("file") && item["file"].isObject()) {
                const auto& fileObj = item["file"];
                if (fileObj.isMember("url")) {
                    url = fileObj["url"].asString();
                }
            }
            
            if (!url.empty()) {
                ImageInfo imgInfo = parseImageUrl(url);
                if (!imgInfo.base64Data.empty() || !imgInfo.uploadedUrl.empty()) {
                    images.push_back(imgInfo);
                    LOG_INFO << "[RequestAdapters] 提取到图片(input_image), mediaType: " << imgInfo.mediaType;
                }
            }
            continue;
        }
        
        // 处理 image_url 类型（兼容 Chat API 格式）
        if (type == "image_url") {
            if (item.isMember("image_url") && item["image_url"].isObject()) {
                const auto& imageUrl = item["image_url"];
                if (imageUrl.isMember("url")) {
                    ImageInfo imgInfo = parseImageUrl(imageUrl["url"].asString());
                    if (!imgInfo.base64Data.empty() || !imgInfo.uploadedUrl.empty()) {
                        images.push_back(imgInfo);
                        LOG_INFO << "[RequestAdapters] 提取到图片(image_url), mediaType: " << imgInfo.mediaType;
                    }
                }
            }
        }
    }
}

std::string RequestAdapters::extractContentText(
    const Json::Value& content,
    std::vector<ImageInfo>& images,
    bool stripZeroWidth
) {
    if (content.isString()) {
        std::string text = content.asString();
        if (stripZeroWidth) {
            ZeroWidthEncoder::stripZeroWidth(text);
        }
        return text;
    }
    
    if (!content.isArray()) {
        return "";
    }
    
    std::string result;
    for (const auto& item : content) {
        if (!item.isObject()) {
            continue;
        }
        
        std::string itemType = item.get("type", "").asString();
        
        // 处理文本类型：支持 "text" (Chat API) 和 "input_text" (Response API)
        if ((itemType == "text" || itemType == "input_text") && 
            item.isMember("text") && item["text"].isString()) {
            std::string textPart = item["text"].asString();
            if (stripZeroWidth) {
                ZeroWidthEncoder::stripZeroWidth(textPart);
            }
            result += textPart;
            if (!textPart.empty() && textPart.back() != '\n') {
                result += "\n";
            }
        }
        // 处理图片类型：image_url (Chat API)
        else if (itemType == "image_url") {
            if (item.isMember("image_url") && item["image_url"].isObject()) {
                const auto& imageUrl = item["image_url"];
                if (imageUrl.isMember("url") && imageUrl["url"].isString()) {
                    ImageInfo imgInfo = parseImageUrl(imageUrl["url"].asString());
                    if (!imgInfo.base64Data.empty() || !imgInfo.uploadedUrl.empty()) {
                        images.push_back(imgInfo);
                    }
                }
            }
        }
        // 处理图片类型：input_image (Response API)
        else if (itemType == "input_image") {
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
                ImageInfo imgInfo = parseImageUrl(url);
                if (!imgInfo.base64Data.empty() || !imgInfo.uploadedUrl.empty()) {
                    images.push_back(imgInfo);
                }
            }
        }
        // 兼容旧格式：直接包含 text 字段的对象
        else if (item.isMember("text") && item["text"].isString()) {
            std::string textPart = item["text"].asString();
            if (stripZeroWidth) {
                ZeroWidthEncoder::stripZeroWidth(textPart);
            }
            result += textPart;
            if (!textPart.empty() && textPart.back() != '\n') {
                result += "\n";
            }
        }
    }
    
    return result;
}

ImageInfo RequestAdapters::parseImageUrl(const std::string& url) {
    ImageInfo imgInfo;
    
    if (url.empty()) {
        return imgInfo;
    }
    
    // 检查是否是 base64 编码的图片
    if (url.find("data:") == 0) {
        // 格式: data:image/png;base64,xxxxx
        size_t semicolonPos = url.find(";");
        size_t commaPos = url.find(",");
        if (semicolonPos != std::string::npos && commaPos != std::string::npos) {
            imgInfo.mediaType = url.substr(5, semicolonPos - 5);  // 提取 image/png
            imgInfo.base64Data = url.substr(commaPos + 1);        // 提取 base64 数据
        }
    } else {
        // 直接是 URL
        imgInfo.uploadedUrl = url;
    }
    
    return imgInfo;
}
