#include "MftRecord.hpp"

#include "Utf.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace {

constexpr std::uint32_t AttributeFileName = 0x30;
constexpr std::uint32_t AttributeData = 0x80;
constexpr std::uint32_t AttributeEnd = 0xFFFFFFFF;
constexpr std::uint16_t InUseFlag = 0x0001;
constexpr std::uint16_t DirectoryFlag = 0x0002;

template <typename T>
std::optional<T> read_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        return std::nullopt;
    }

    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(bytes[offset + i]) << (8U * i);
    }
    return value;
}

std::uint64_t base_file_ref(std::uint64_t file_ref) {
    return file_ref & 0x0000FFFFFFFFFFFFULL;
}

std::optional<std::vector<std::uint8_t>> restored_record(const std::vector<std::uint8_t>& input) {
    if (input.size() != MftRecordParser::RecordSize || std::memcmp(input.data(), "FILE", 4) != 0) {
        return std::nullopt;
    }

    auto bytes = input;
    if (!MftRecordParser::restore_usa(bytes)) {
        return std::nullopt;
    }
    return bytes;
}

std::int64_t sign_extend(std::uint64_t value, std::uint8_t byte_count) {
    if (byte_count == 0 || byte_count >= 8) {
        return static_cast<std::int64_t>(value);
    }

    const std::uint64_t sign_bit = 1ULL << ((byte_count * 8U) - 1U);
    if ((value & sign_bit) == 0) {
        return static_cast<std::int64_t>(value);
    }

    const std::uint64_t mask = ~((1ULL << (byte_count * 8U)) - 1ULL);
    return static_cast<std::int64_t>(value | mask);
}

bool checked_add(std::int64_t lhs, std::int64_t rhs, std::int64_t& result) {
    if ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs)) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

std::optional<std::vector<DataRun>> parse_data_runs(const std::vector<std::uint8_t>& bytes,
                                                    std::size_t offset,
                                                    std::size_t end) {
    std::vector<DataRun> runs;
    std::int64_t current_lcn = 0;

    while (offset < end) {
        const std::uint8_t header = bytes[offset++];
        if (header == 0) {
            return runs;
        }

        const std::uint8_t length_size = header & 0x0FU;
        const std::uint8_t offset_size = (header >> 4U) & 0x0FU;
        if (length_size == 0 || length_size > 8 || offset_size > 8 ||
            offset + length_size + offset_size > end) {
            return std::nullopt;
        }

        std::uint64_t cluster_count = 0;
        for (std::uint8_t i = 0; i < length_size; ++i) {
            cluster_count |= static_cast<std::uint64_t>(bytes[offset + i]) << (8U * i);
        }
        offset += length_size;

        std::uint64_t raw_delta = 0;
        for (std::uint8_t i = 0; i < offset_size; ++i) {
            raw_delta |= static_cast<std::uint64_t>(bytes[offset + i]) << (8U * i);
        }
        offset += offset_size;

        DataRun run;
        run.cluster_count = cluster_count;
        run.sparse = offset_size == 0;
        if (!run.sparse) {
            std::int64_t next_lcn = 0;
            if (!checked_add(current_lcn, sign_extend(raw_delta, offset_size), next_lcn) ||
                next_lcn < 0) {
                return std::nullopt;
            }
            current_lcn = next_lcn;
            run.lcn = static_cast<std::uint64_t>(current_lcn);
        }
        runs.push_back(run);
    }

    return std::nullopt;
}

} // namespace

std::optional<MftRecord> MftRecordParser::parse(const std::vector<std::uint8_t>& input,
                                                std::uint64_t offset) const {
    if (input.size() != RecordSize || std::memcmp(input.data(), "FILE", 4) != 0) {
        return std::nullopt;
    }

    auto bytes = input;
    if (!restore_usa(bytes)) {
        return std::nullopt;
    }

    const auto sequence_number = read_le<std::uint16_t>(bytes, 0x10);
    const auto flags = read_le<std::uint16_t>(bytes, 0x16);
    const auto first_attr_offset = read_le<std::uint16_t>(bytes, 0x14);
    const auto record_number = read_le<std::uint32_t>(bytes, 0x2C);
    if (!sequence_number || !flags || !first_attr_offset || !record_number) {
        return std::nullopt;
    }

    MftRecord record;
    record.offset = offset;
    record.record_id_guess = *record_number != 0
        ? *record_number
        : offset / RecordSize;

    const bool record_is_directory = (*flags & DirectoryFlag) != 0;
    std::size_t attr_offset = *first_attr_offset;

    while (attr_offset + 16 <= bytes.size()) {
        const auto attr_type = read_le<std::uint32_t>(bytes, attr_offset);
        if (!attr_type || *attr_type == AttributeEnd) {
            break;
        }

        const auto attr_len = read_le<std::uint32_t>(bytes, attr_offset + 4);
        const auto non_resident = read_le<std::uint8_t>(bytes, attr_offset + 8);
        if (!attr_len || *attr_len < 16 || attr_offset + *attr_len > bytes.size()) {
            break;
        }

        if (*attr_type == AttributeFileName && non_resident && *non_resident == 0) {
            const auto content_len = read_le<std::uint32_t>(bytes, attr_offset + 16);
            const auto content_offset = read_le<std::uint16_t>(bytes, attr_offset + 20);
            if (content_len && content_offset) {
                const std::size_t content_start = attr_offset + *content_offset;
                const std::size_t content_end = content_start + *content_len;
                if (content_start + 66 <= bytes.size() && content_end <= bytes.size()) {
                    const auto parent = read_le<std::uint64_t>(bytes, content_start);
                    const auto allocated = read_le<std::uint64_t>(bytes, content_start + 40);
                    const auto real = read_le<std::uint64_t>(bytes, content_start + 48);
                    const auto name_flags = read_le<std::uint32_t>(bytes, content_start + 56);
                    const auto name_len = read_le<std::uint8_t>(bytes, content_start + 64);
                    const auto ns = read_le<std::uint8_t>(bytes, content_start + 65);

                    if (parent && allocated && real && name_flags && name_len && ns) {
                        const std::size_t name_bytes = static_cast<std::size_t>(*name_len) * 2;
                        const std::size_t name_start = content_start + 66;
                        if (name_start + name_bytes <= content_end) {
                            MftFileName file_name;
                            file_name.parent_ref = base_file_ref(*parent);
                            file_name.allocated_size = *allocated;
                            file_name.real_size = *real;
                            file_name.flags = *name_flags;
                            file_name.name_namespace = *ns;
                            file_name.is_directory = record_is_directory ||
                                ((*name_flags & 0x10000000U) != 0);
                            file_name.name = utf16le_to_utf8(bytes.data() + name_start, name_bytes);

                            if (!file_name.name.empty()) {
                                record.names.push_back(std::move(file_name));
                            }
                        }
                    }
                }
            }
        }

        attr_offset += *attr_len;
    }

    if (record.names.empty()) {
        return std::nullopt;
    }

    (void)InUseFlag;
    return record;
}

bool MftRecordParser::restore_usa(std::vector<std::uint8_t>& bytes) {
    const auto usa_offset = read_le<std::uint16_t>(bytes, 0x04);
    const auto usa_count = read_le<std::uint16_t>(bytes, 0x06);
    if (!usa_offset || !usa_count || *usa_count < 2) {
        return false;
    }

    const std::size_t usa_bytes = static_cast<std::size_t>(*usa_count) * sizeof(std::uint16_t);
    if (*usa_offset > bytes.size() || usa_bytes > bytes.size() - *usa_offset) {
        return false;
    }

    constexpr std::size_t sector_size = 512;
    const std::size_t sector_count = bytes.size() / sector_size;
    if (*usa_count != sector_count + 1) {
        return false;
    }

    const auto update_sequence = read_le<std::uint16_t>(bytes, *usa_offset);
    if (!update_sequence) {
        return false;
    }

    for (std::size_t sector = 0; sector < sector_count; ++sector) {
        const std::size_t trailer_offset = ((sector + 1) * sector_size) - sizeof(std::uint16_t);
        const auto current = read_le<std::uint16_t>(bytes, trailer_offset);
        const auto replacement = read_le<std::uint16_t>(
            bytes,
            *usa_offset + ((sector + 1) * sizeof(std::uint16_t)));

        if (!current || !replacement || *current != *update_sequence) {
            return false;
        }

        bytes[trailer_offset] = static_cast<std::uint8_t>(*replacement & 0xFFU);
        bytes[trailer_offset + 1] = static_cast<std::uint8_t>((*replacement >> 8U) & 0xFFU);
    }

    return true;
}

std::optional<std::uint64_t> MftRecordParser::record_number(const std::vector<std::uint8_t>& input) const {
    const auto bytes = restored_record(input);
    if (!bytes) {
        return std::nullopt;
    }

    const auto record_number = read_le<std::uint32_t>(*bytes, 0x2C);
    if (!record_number) {
        return std::nullopt;
    }
    return *record_number;
}

std::vector<DataAttribute> MftRecordParser::data_attributes(const std::vector<std::uint8_t>& input) const {
    std::vector<DataAttribute> attributes;
    const auto bytes = restored_record(input);
    if (!bytes) {
        return attributes;
    }

    const auto first_attr_offset = read_le<std::uint16_t>(*bytes, 0x14);
    if (!first_attr_offset) {
        return attributes;
    }

    std::size_t attr_offset = *first_attr_offset;
    while (attr_offset + 16 <= bytes->size()) {
        const auto attr_type = read_le<std::uint32_t>(*bytes, attr_offset);
        if (!attr_type || *attr_type == AttributeEnd) {
            break;
        }

        const auto attr_len = read_le<std::uint32_t>(*bytes, attr_offset + 4);
        const auto non_resident = read_le<std::uint8_t>(*bytes, attr_offset + 8);
        const auto name_len = read_le<std::uint8_t>(*bytes, attr_offset + 9);
        const auto attr_flags = read_le<std::uint16_t>(*bytes, attr_offset + 12);
        if (!attr_len || *attr_len < 16 || attr_offset + *attr_len > bytes->size() ||
            !non_resident || !name_len || !attr_flags) {
            break;
        }

        const std::size_t attr_end = attr_offset + *attr_len;
        if (*attr_type == AttributeData && *name_len == 0 && (*attr_flags & 0x4001U) == 0) {
            if (*non_resident == 0) {
                const auto content_len = read_le<std::uint32_t>(*bytes, attr_offset + 16);
                const auto content_offset = read_le<std::uint16_t>(*bytes, attr_offset + 20);
                if (content_len && content_offset) {
                    const std::size_t content_start = attr_offset + *content_offset;
                    if (content_start <= attr_end && *content_len <= attr_end - content_start) {
                        DataAttribute data;
                        data.resident = true;
                        data.real_size = *content_len;
                        data.resident_data.assign(bytes->begin() + static_cast<std::ptrdiff_t>(content_start),
                                                  bytes->begin() + static_cast<std::ptrdiff_t>(content_start + *content_len));
                        attributes.push_back(std::move(data));
                    }
                }
            } else if (*non_resident == 1) {
                const auto run_offset = read_le<std::uint16_t>(*bytes, attr_offset + 32);
                const auto real_size = read_le<std::uint64_t>(*bytes, attr_offset + 48);
                if (run_offset && real_size) {
                    const std::size_t run_start = attr_offset + *run_offset;
                    if (run_start < attr_end) {
                        auto runs = parse_data_runs(*bytes, run_start, attr_end);
                        if (runs) {
                            DataAttribute data;
                            data.resident = false;
                            data.real_size = *real_size;
                            data.runs = std::move(*runs);
                            attributes.push_back(std::move(data));
                        }
                    }
                }
            }
        }

        attr_offset += *attr_len;
    }

    return attributes;
}
