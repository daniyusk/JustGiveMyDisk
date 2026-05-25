#include "Utf.hpp"

#include <string>

namespace {

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6U)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3FU)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12U)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18U)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3FU)));
    }
}

std::uint16_t read_u16le(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0]) |
        (static_cast<std::uint16_t>(data[1]) << 8U);
}

} // namespace

std::string utf16le_to_utf8(const std::uint8_t* data, std::size_t byte_count) {
    std::string out;
    out.reserve(byte_count);

    for (std::size_t i = 0; i + 1 < byte_count; i += 2) {
        std::uint32_t cp = read_u16le(data + i);

        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < byte_count) {
            const std::uint32_t low = read_u16le(data + i + 2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + (((cp - 0xD800) << 10U) | (low - 0xDC00));
                i += 2;
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }

        if (cp == 0) {
            continue;
        }
        append_utf8(out, cp);
    }

    return out;
}
