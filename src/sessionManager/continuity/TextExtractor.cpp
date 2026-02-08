#include "sessionManager/continuity/TextExtractor.h"
std::vector<std::string> TextExtractor::extractForContinuity(const GenerationRequest& req) {
    if (!req.continuityTexts.empty()) {
        return req.continuityTexts;
    }

    std::vector<std::string> out;
    out.reserve(req.messages.size() + 1);

    for (const auto& msg : req.messages) {
        const std::string text = msg.getTextContent();
        if (!text.empty()) out.push_back(text);
    }

    if (!req.currentInput.empty()) {
        out.push_back(req.currentInput);
    }

    return out;
}

