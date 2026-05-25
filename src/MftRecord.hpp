#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct MftFileName {
    std::uint64_t parent_ref = 0;
    std::string name;
    std::uint8_t name_namespace = 0;
    std::uint32_t flags = 0;
    std::uint64_t allocated_size = 0;
    std::uint64_t real_size = 0;
    bool is_directory = false;
};

struct MftRecord {
    std::uint64_t record_id_guess = 0;
    std::uint64_t offset = 0;
    std::vector<MftFileName> names;
};

class MftRecordParser {
public:
    static constexpr std::size_t RecordSize = 1024;

    std::optional<MftRecord> parse(const std::vector<std::uint8_t>& bytes,
                                   std::uint64_t offset) const;

private:
    static bool restore_usa(std::vector<std::uint8_t>& bytes);
};
