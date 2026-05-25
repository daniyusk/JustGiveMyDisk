#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

std::string utf16le_to_utf8(const std::uint8_t* data, std::size_t byte_count);
