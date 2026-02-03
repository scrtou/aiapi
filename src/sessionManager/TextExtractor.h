#ifndef TEXT_EXTRACTOR_H
#define TEXT_EXTRACTOR_H

#include <string>
#include <vector>

#include "GenerationRequest.h"

/**
 * @brief TextExtractor
 *
 * 将 GenerationRequest 中可能包含会话连续性信息的文本统一抽取为文本集合。
 *
 * 说明：
 * - RequestAdapters 会尽量填充 GenerationRequest.continuityTexts（保留零宽字符）。
 * - 若该字段为空，则回退从 messages/currentInput 组合抽取。
 */
class TextExtractor {
public:
    static std::vector<std::string> extractForContinuity(const GenerationRequest& req);
};

#endif // TEXT_EXTRACTOR_H

