#include "ZeroWidthEncoder.h"
#include <sstream>
#include <cstring>

// 静态成员初始化
const std::string ZeroWidthEncoder::START_MARKER = std::string("\xE2\x81\xA0\xE2\x81\xA0"); // U+2060 U+2060
const std::string ZeroWidthEncoder::END_MARKER = std::string("\xE2\x81\xA0\xE2\x81\xA0");   // U+2060 U+2060

std::string ZeroWidthEncoder::encode(const std::string& data) {
    if (data.empty()) {
        return "";
    }
    
    std::string result = START_MARKER;
    
    for (unsigned char c : data) {
        result += encodeByte(c);
    }
    
    result += END_MARKER;
    return result;
}

std::optional<std::string> ZeroWidthEncoder::decode(const std::string& text) {
    // 查找起始标记
    size_t startPos = text.find(START_MARKER);
    if (startPos == std::string::npos) {
        return std::nullopt;
    }
    
    // 跳过起始标记
    size_t dataStart = startPos + START_MARKER.length();
    
    // 查找结束标记
    size_t endPos = text.find(END_MARKER, dataStart);
    if (endPos == std::string::npos) {
        return std::nullopt;
    }
    
    // 提取编码数据
    std::string encodedData = text.substr(dataStart, endPos - dataStart);
    
    // 解码
    std::string result;
    
    // 每个字节由4个零宽字符表示，每个零宽字符是3字节UTF-8
    const size_t zwCharLen = 3;  // UTF-8编码的零宽字符长度
    const size_t charsPerByte = 4;  // 每个字节需要4个零宽字符
    const size_t bytesPerChar = zwCharLen * charsPerByte;  // 每个原始字节需要的UTF-8字节数
    
    if (encodedData.length() % bytesPerChar != 0) {
        return std::nullopt;  // 数据长度不正确
    }
    
    for (size_t i = 0; i < encodedData.length(); i += bytesPerChar) {
        std::string zwChars = encodedData.substr(i, bytesPerChar);
        unsigned char byte;
        if (!decodeByte(zwChars, byte)) {
            return std::nullopt;
        }
        result += static_cast<char>(byte);
    }
    
    return result;
}

std::optional<std::string> ZeroWidthEncoder::extractAndRemove(std::string& text) {
    // 查找起始标记
    size_t startPos = text.find(START_MARKER);
    if (startPos == std::string::npos) {
        return std::nullopt;
    }
    
    // 跳过起始标记
    size_t dataStart = startPos + START_MARKER.length();
    
    // 查找结束标记
    size_t endPos = text.find(END_MARKER, dataStart);
    if (endPos == std::string::npos) {
        return std::nullopt;
    }
    
    // 提取编码数据
    std::string encodedData = text.substr(dataStart, endPos - dataStart);
    
    // 解码
    std::string result;
    
    const size_t zwCharLen = 3;
    const size_t charsPerByte = 4;
    const size_t bytesPerChar = zwCharLen * charsPerByte;
    
    if (encodedData.length() % bytesPerChar != 0) {
        return std::nullopt;
    }
    
    for (size_t i = 0; i < encodedData.length(); i += bytesPerChar) {
        std::string zwChars = encodedData.substr(i, bytesPerChar);
        unsigned char byte;
        if (!decodeByte(zwChars, byte)) {
            return std::nullopt;
        }
        result += static_cast<char>(byte);
    }
    
    // 移除整个编码序列（包括标记）
    size_t totalEnd = endPos + END_MARKER.length();
    text.erase(startPos, totalEnd - startPos);
    
    return result;
}

std::string ZeroWidthEncoder::appendEncoded(const std::string& text, const std::string& data) {
    return text + encode(data);
}

bool ZeroWidthEncoder::hasEncodedData(const std::string& text) {
    size_t startPos = text.find(START_MARKER);
    if (startPos == std::string::npos) {
        return false;
    }
    
    size_t endPos = text.find(END_MARKER, startPos + START_MARKER.length());
    return endPos != std::string::npos;
}

std::string ZeroWidthEncoder::stripZeroWidth(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    
    size_t i = 0;
    while (i < text.size()) {
        // 检查是否是零宽字符 (3字节UTF-8)
        if (i + 2 < text.size()) {
            std::string potential = text.substr(i, 3);
            
            // 检查是否是任何零宽字符
            if (potential == ZW_SPACE || 
                potential == ZW_NON_JOINER || 
                potential == ZW_JOINER || 
                potential == ZW_NO_BREAK ||
                potential == MARKER) {
                i += 3;  // 跳过零宽字符
                continue;
            }
        }
        
        result += text[i];
        ++i;
    }
    
    return result;
}

std::string ZeroWidthEncoder::encodeByte(unsigned char byte) {
    std::string result;
    
    // 每2位编码为一个零宽字符，从高位到低位
    for (int shift = 6; shift >= 0; shift -= 2) {
        int value = (byte >> shift) & 0x03;  // 提取2位
        result += getZwChar(value);
    }
    
    return result;
}

bool ZeroWidthEncoder::decodeByte(const std::string& zwChars, unsigned char& outByte) {
    const size_t zwCharLen = 3;  // UTF-8编码的零宽字符长度
    
    if (zwChars.length() != zwCharLen * 4) {
        return false;
    }
    
    outByte = 0;
    
    for (int i = 0; i < 4; ++i) {
        std::string zwChar = zwChars.substr(i * zwCharLen, zwCharLen);
        int value = getZwValue(zwChar);
        if (value < 0) {
            return false;
        }
        outByte = (outByte << 2) | static_cast<unsigned char>(value);
    }
    
    return true;
}

int ZeroWidthEncoder::getZwValue(const std::string& zwChar) {
    if (zwChar == ZW_SPACE) return 0;
    if (zwChar == ZW_NON_JOINER) return 1;
    if (zwChar == ZW_JOINER) return 2;
    if (zwChar == ZW_NO_BREAK) return 3;
    return -1;
}

const char* ZeroWidthEncoder::getZwChar(int value) {
    switch (value & 0x03) {
        case 0: return ZW_SPACE;
        case 1: return ZW_NON_JOINER;
        case 2: return ZW_JOINER;
        case 3: return ZW_NO_BREAK;
        default: return ZW_SPACE;  // 不应该到达这里
    }
}
