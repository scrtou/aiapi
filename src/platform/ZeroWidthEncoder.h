#ifndef ZERO_WIDTH_ENCODER_H
#define ZERO_WIDTH_ENCODER_H

#include <string>
#include <optional>

/**
 * @brief 零宽字符编码器
 * 
 * 使用不可见的零宽字符将 session ID 隐式嵌入到响应文本中。
 * 
 * 编码方案：
 * - 使用 4 个零宽字符来表示 2 位二进制数据
 * - U+200B (零宽空格) = 00
 * - U+200C (零宽非连接符) = 01
 * - U+200D (零宽连接符) = 10
 * - U+FEFF (零宽非断空格) = 11
 * 
 * 格式：
 * - 起始标记：U+2060 U+2060 (词连接符 x2)
 * - 编码数据：零宽字符序列
 * - 结束标记：U+2060 U+2060
 * 
 * 这种方式对用户完全不可见，但可以被程序解析。
 */
class ZeroWidthEncoder {
public:
    /**
     * @brief 将字符串编码为零宽字符序列
     * 
     * @param data 要编码的数据（如 session ID）
     * @return 编码后的零宽字符序列（包含起始和结束标记）
     */
    static std::string encode(const std::string& data);
    
    /**
     * @brief 从文本中解码零宽字符序列
     * 
     * @param text 包含零宽字符的文本
     * @return 解码后的数据，如果没有找到有效的编码则返回 nullopt
     */
    static std::optional<std::string> decode(const std::string& text);
    
    /**
     * @brief 从文本中提取并移除零宽字符编码的数据
     * 
     * @param text 包含零宽字符的文本（会被修改，移除零宽字符序列）
     * @return 解码后的数据，如果没有找到有效的编码则返回 nullopt
     */
    static std::optional<std::string> extractAndRemove(std::string& text);
    
    /**
     * @brief 将编码后的数据追加到文本末尾
     * 
     * @param text 原始文本
     * @param data 要嵌入的数据
     * @return 带有嵌入数据的文本
     */
    static std::string appendEncoded(const std::string& text, const std::string& data);
    
    /**
     * @brief 检查文本是否包含零宽字符编码
     * 
     * @param text 要检查的文本
     * @return true 如果包含有效的编码标记
     */
    static bool hasEncodedData(const std::string& text);
    
    /**
     * @brief 移除文本中所有的零宽字符（清洗文本）
     * 
     * @param text 要清洗的文本
     * @return 移除零宽字符后的文本
     */
    static std::string stripZeroWidth(const std::string& text);
    
private:
    // 零宽字符常量 (UTF-8 编码)
    static constexpr const char* ZW_SPACE = "\xE2\x80\x8B";      // U+200B 零宽空格 = 00
    static constexpr const char* ZW_NON_JOINER = "\xE2\x80\x8C"; // U+200C 零宽非连接符 = 01
    static constexpr const char* ZW_JOINER = "\xE2\x80\x8D";     // U+200D 零宽连接符 = 10
    static constexpr const char* ZW_NO_BREAK = "\xEF\xBB\xBF";   // +FEFF 零宽非断空格 = 11
    static constexpr const char* MARKER = "\xE2\x81\xA0";        // U+2060 词连接符（用作标记）
    
    // 起始和结束标记
    static const std::string START_MARKER;
    static const std::string END_MARKER;
    
    /**
     * @brief 将单个字节编码为零宽字符序列
     * 
     * @param byte 要编码的字节
     * @return 4个零宽字符（每个代表2位）
     */
    static std::string encodeByte(unsigned char byte);
    
    /**
     * @brief 从零宽字符序列解码单个字节
     * 
     * @param zwChars 4个零宽字符
     * @param outByte 输出的字节
     * @return true 如果解码成功
     */
    static bool decodeByte(const std::string& zwChars, unsigned char& outByte);
    
    /**
     * @brief 获取零宽字符代表的2位值
     * 
     * @param zwChar 零宽字符（UTF-8，3字节）
     * @return 0-3 的值，或 -1 如果不是有效的零宽字符
     */
    static int getZwValue(const std::string& zwChar);
    
    /**
     * @brief 获取2位值对应的零宽字符
     * 
     * @param value 0-3 的值
     * @return 对应的零宽字符
     */
    static const char* getZwChar(int value);
};

#endif // 头文件保护结束
