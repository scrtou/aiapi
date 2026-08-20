#pragma once

#include <domain/model/AiApiData.h>

#include <functional>
#include <memory>
#include <string>

#include <json/json.h>

class IResponseSink;

namespace aiapi {

/**
 * 向 AiApiController 提供的唯一运行时协作接口。
 *
 * 控制器负责 HTTP 校验和 IO 回调；本用例负责请求规范化、队列准入、生成任务执行、
 * 提供商模型目录查询，以及 Responses 数据的持久化、读取和删除流程。
 */
class IAiApiUseCase
{
  public:
    using Completion = std::function<void(
        const GenerationResult&, const std::shared_ptr<IResponseSink>&)>;

    /**
     * 传递给协议层响应接收器工厂的传输回调。
     * 控制器只提供 IO 绑定，不负责构造具体的 Chat/Responses 响应接收器。
     */
    struct ResponseBinding {
        bool stream = false;
        std::function<void(const Json::Value&, int)> jsonResponse;
        std::function<bool(const std::string&)> streamWriter;
        std::function<void()> close;
    };

    virtual ~IAiApiUseCase() = default;

    virtual SubmissionResult submitGeneration(
        GenerationInput input, ResponseBinding binding, Completion onComplete) = 0;

    virtual ModelCatalogResult modelCatalog(const std::string& provider) const = 0;
    virtual StoredResponseResult getResponse(const std::string& responseId) = 0;
    virtual DeleteResponseResult deleteResponse(const std::string& responseId) = 0;
};

}  // aiapi 命名空间
