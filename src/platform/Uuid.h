#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>

namespace platform {

/** Generate a process-local RFC 4122 version-4 UUID without an HTTP framework. */
inline std::string generateUuidV4()
{
    thread_local std::mt19937_64 engine([] {
        std::random_device device;
        const auto now = static_cast<std::uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        const auto thread = static_cast<std::uint64_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
        std::seed_seq seed{
            device(), device(),
            static_cast<unsigned int>(now), static_cast<unsigned int>(now >> 32),
            static_cast<unsigned int>(thread), static_cast<unsigned int>(thread >> 32),
        };
        return std::mt19937_64(seed);
    }());

    std::array<unsigned char, 16> bytes{};
    for (auto& byte : bytes) {
        byte = static_cast<unsigned char>(engine() & 0xffU);
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);

    std::ostringstream value;
    value << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        value << std::setw(2) << static_cast<unsigned int>(bytes[index]);
        if (index == 3 || index == 5 || index == 7 || index == 9) {
            value << '-';
        }
    }
    return value.str();
}

}  // namespace platform
