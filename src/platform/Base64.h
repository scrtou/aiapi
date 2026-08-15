#pragma once

#include <cstddef>
#include <string>

namespace platform {

inline std::string base64Encode(const std::string& input)
{
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);

    std::size_t index = 0;
    while (index + 3 <= input.size()) {
        const unsigned int value =
            (static_cast<unsigned char>(input[index]) << 16U) |
            (static_cast<unsigned char>(input[index + 1]) << 8U) |
            static_cast<unsigned char>(input[index + 2]);
        output.push_back(kAlphabet[(value >> 18U) & 0x3fU]);
        output.push_back(kAlphabet[(value >> 12U) & 0x3fU]);
        output.push_back(kAlphabet[(value >> 6U) & 0x3fU]);
        output.push_back(kAlphabet[value & 0x3fU]);
        index += 3;
    }

    const std::size_t remaining = input.size() - index;
    if (remaining == 1) {
        const unsigned int value = static_cast<unsigned char>(input[index]) << 16U;
        output.push_back(kAlphabet[(value >> 18U) & 0x3fU]);
        output.push_back(kAlphabet[(value >> 12U) & 0x3fU]);
        output += "==";
    } else if (remaining == 2) {
        const unsigned int value =
            (static_cast<unsigned char>(input[index]) << 16U) |
            (static_cast<unsigned char>(input[index + 1]) << 8U);
        output.push_back(kAlphabet[(value >> 18U) & 0x3fU]);
        output.push_back(kAlphabet[(value >> 12U) & 0x3fU]);
        output.push_back(kAlphabet[(value >> 6U) & 0x3fU]);
        output.push_back('=');
    }
    return output;
}

}  // namespace platform
