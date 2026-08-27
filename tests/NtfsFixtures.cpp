#include "NtfsFixtures.hpp"

#include "MftRecord.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ntfs::test {
namespace {

constexpr std::size_t UsaOffset = 0x30;
constexpr std::size_t FirstAttributeOffset = 0x38;
constexpr std::uint16_t UpdateSequence = 0xA55A;
constexpr std::uint32_t AttributeFileName = 0x30;
constexpr std::uint32_t AttributeData = 0x80;
constexpr std::uint32_t AttributeEnd = 0xFFFFFFFF;

template <typename T>
void put_le(Bytes& bytes, std::size_t offset, T value) {
    static_assert(std::is_integral_v<T>);
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        throw std::out_of_range("fixture write outside record");
    }
    using Unsigned = std::make_unsigned_t<T>;
    const auto raw = static_cast<Unsigned>(value);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(raw >> (8U * i));
    }
}

template <typename T>
T get_le(const Bytes& bytes, std::size_t offset) {
    using Unsigned = std::make_unsigned_t<T>;
    Unsigned value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<Unsigned>(bytes.at(offset + i)) << (8U * i);
    }
    return static_cast<T>(value);
}

std::size_t align8(std::size_t value) {
    return (value + 7U) & ~std::size_t{7U};
}

Bytes utf16le(const std::u16string& code_units) {
    Bytes result;
    result.reserve(code_units.size() * 2U);
    for (const char16_t code_unit : code_units) {
        result.push_back(static_cast<std::uint8_t>(code_unit & 0xFFU));
        result.push_back(static_cast<std::uint8_t>((code_unit >> 8U) & 0xFFU));
    }
    return result;
}

std::size_t unsigned_width(std::uint64_t value) {
    std::size_t width = 1;
    while (width < sizeof(value) && (value >> (width * 8U)) != 0) {
        ++width;
    }
    return width;
}

std::size_t signed_width(std::int64_t value) {
    for (std::size_t width = 1; width < sizeof(value); ++width) {
        const auto bits = width * 8U;
        const auto minimum = -(std::int64_t{1} << (bits - 1U));
        const auto maximum = (std::int64_t{1} << (bits - 1U)) - 1;
        if (value >= minimum && value <= maximum) {
            return width;
        }
    }
    return sizeof(value);
}

Bytes encode_runs(const std::vector<DataRunSpec>& runs) {
    Bytes result;
    for (const auto& run : runs) {
        const std::size_t length_width = unsigned_width(run.cluster_count);
        const std::size_t offset_width = run.lcn_delta ? signed_width(*run.lcn_delta) : 0;
        result.push_back(static_cast<std::uint8_t>((offset_width << 4U) | length_width));

        for (std::size_t i = 0; i < length_width; ++i) {
            result.push_back(static_cast<std::uint8_t>(run.cluster_count >> (8U * i)));
        }
        if (run.lcn_delta) {
            const auto raw = static_cast<std::uint64_t>(*run.lcn_delta);
            for (std::size_t i = 0; i < offset_width; ++i) {
                result.push_back(static_cast<std::uint8_t>(raw >> (8U * i)));
            }
        }
    }
    result.push_back(0);
    return result;
}

std::size_t find_attribute(const Bytes& bytes, std::uint32_t wanted_type) {
    std::size_t offset = get_le<std::uint16_t>(bytes, 0x14);
    while (offset + 16U <= bytes.size()) {
        const auto type = get_le<std::uint32_t>(bytes, offset);
        if (type == wanted_type) {
            return offset;
        }
        if (type == AttributeEnd) {
            break;
        }
        const auto length = get_le<std::uint32_t>(bytes, offset + 4U);
        if (length < 16U || length > bytes.size() - offset) {
            break;
        }
        offset += length;
    }
    throw std::logic_error("fixture attribute not found");
}

void apply_usa(Bytes& bytes, bool corrupt) {
    put_le<std::uint16_t>(bytes, UsaOffset, UpdateSequence);
    for (std::size_t sector = 0; sector < 2; ++sector) {
        const std::size_t trailer = ((sector + 1U) * 512U) - 2U;
        put_le<std::uint16_t>(bytes, UsaOffset + 2U + (sector * 2U),
                              get_le<std::uint16_t>(bytes, trailer));
        put_le<std::uint16_t>(bytes, trailer, UpdateSequence);
    }
    if (corrupt) {
        bytes[510] ^= 0x01U;
    }
}

} // namespace

DataRunSpec positive_run(std::uint64_t cluster_count, std::int64_t lcn_delta) {
    if (lcn_delta < 0) {
        throw std::invalid_argument("positive data run requires a non-negative delta");
    }
    return {cluster_count, lcn_delta};
}

DataRunSpec negative_run(std::uint64_t cluster_count, std::int64_t lcn_delta) {
    if (lcn_delta >= 0) {
        throw std::invalid_argument("negative data run requires a negative delta");
    }
    return {cluster_count, lcn_delta};
}

DataRunSpec sparse_run(std::uint64_t cluster_count) {
    return {cluster_count, std::nullopt};
}

FileRecordBuilder::FileRecordBuilder()
    : bytes_(MftRecordParser::RecordSize, 0), attribute_offset_(FirstAttributeOffset) {
    std::memcpy(bytes_.data(), "FILE", 4);
    put_le<std::uint16_t>(bytes_, 0x04, UsaOffset);
    put_le<std::uint16_t>(bytes_, 0x06, 3);
    put_le<std::uint16_t>(bytes_, 0x10, 1);
    put_le<std::uint16_t>(bytes_, 0x14, FirstAttributeOffset);
    put_le<std::uint16_t>(bytes_, 0x16, 1);
    put_le<std::uint32_t>(bytes_, 0x1C, MftRecordParser::RecordSize);
    put_le<std::uint32_t>(bytes_, 0x2C, 42);
}

FileRecordBuilder& FileRecordBuilder::record_number(std::uint32_t value) {
    put_le<std::uint32_t>(bytes_, 0x2C, value);
    return *this;
}

FileRecordBuilder& FileRecordBuilder::flags(std::uint16_t value) {
    put_le<std::uint16_t>(bytes_, 0x16, value);
    return *this;
}

void FileRecordBuilder::append_resident_attribute(std::uint32_t type, const Bytes& content) {
    constexpr std::size_t HeaderSize = 24;
    const std::size_t attribute_size = align8(HeaderSize + content.size());
    if (attribute_offset_ > bytes_.size() || attribute_size + 4U > bytes_.size() - attribute_offset_) {
        throw std::length_error("fixture attributes do not fit in one FILE record");
    }

    put_le<std::uint32_t>(bytes_, attribute_offset_, type);
    put_le<std::uint32_t>(bytes_, attribute_offset_ + 4U,
                          static_cast<std::uint32_t>(attribute_size));
    bytes_[attribute_offset_ + 8U] = 0;
    put_le<std::uint32_t>(bytes_, attribute_offset_ + 16U,
                          static_cast<std::uint32_t>(content.size()));
    put_le<std::uint16_t>(bytes_, attribute_offset_ + 20U, HeaderSize);
    std::copy(content.begin(), content.end(),
              bytes_.begin() + static_cast<std::ptrdiff_t>(attribute_offset_ + HeaderSize));
    attribute_offset_ += attribute_size;
}

FileRecordBuilder& FileRecordBuilder::file_name(std::u16string code_units,
                                                std::uint64_t parent_ref,
                                                std::uint8_t name_namespace) {
    if (code_units.size() > std::numeric_limits<std::uint8_t>::max()) {
        throw std::length_error("fixture filename exceeds the NTFS one-byte length field");
    }

    Bytes content(66, 0);
    put_le<std::uint64_t>(content, 0, parent_ref);
    put_le<std::uint64_t>(content, 40, 4096);
    put_le<std::uint64_t>(content, 48, 1234);
    content[64] = static_cast<std::uint8_t>(code_units.size());
    content[65] = name_namespace;
    const auto name_bytes = utf16le(code_units);
    content.insert(content.end(), name_bytes.begin(), name_bytes.end());
    append_resident_attribute(AttributeFileName, content);
    return *this;
}

FileRecordBuilder& FileRecordBuilder::resident_data(Bytes data) {
    append_resident_attribute(AttributeData, data);
    return *this;
}

FileRecordBuilder& FileRecordBuilder::non_resident_data(std::vector<DataRunSpec> runs,
                                                        std::uint64_t real_size) {
    constexpr std::size_t HeaderSize = 64;
    const Bytes run_bytes = encode_runs(runs);
    const std::size_t attribute_size = align8(HeaderSize + run_bytes.size());
    if (attribute_offset_ > bytes_.size() || attribute_size + 4U > bytes_.size() - attribute_offset_) {
        throw std::length_error("fixture attributes do not fit in one FILE record");
    }

    put_le<std::uint32_t>(bytes_, attribute_offset_, AttributeData);
    put_le<std::uint32_t>(bytes_, attribute_offset_ + 4U,
                          static_cast<std::uint32_t>(attribute_size));
    bytes_[attribute_offset_ + 8U] = 1;
    put_le<std::uint16_t>(bytes_, attribute_offset_ + 32U, HeaderSize);
    put_le<std::uint64_t>(bytes_, attribute_offset_ + 48U, real_size);
    std::copy(run_bytes.begin(), run_bytes.end(),
              bytes_.begin() + static_cast<std::ptrdiff_t>(attribute_offset_ + HeaderSize));
    attribute_offset_ += attribute_size;
    return *this;
}

FileRecordBuilder& FileRecordBuilder::invalid_usa() {
    invalid_usa_ = true;
    return *this;
}

Bytes FileRecordBuilder::build() const {
    Bytes result = bytes_;
    put_le<std::uint32_t>(result, attribute_offset_, AttributeEnd);
    put_le<std::uint32_t>(result, 0x18, static_cast<std::uint32_t>(attribute_offset_ + 4U));
    apply_usa(result, invalid_usa_);
    return result;
}

Bytes valid_file_record(std::u16string name) {
    return FileRecordBuilder{}.file_name(std::move(name)).build();
}

Bytes valid_usa_record() {
    return valid_file_record();
}

Bytes invalid_usa_record() {
    return FileRecordBuilder{}.file_name(u"bad-usa.txt").invalid_usa().build();
}

Bytes resident_data_record(Bytes data) {
    return FileRecordBuilder{}.file_name(u"resident.bin").resident_data(std::move(data)).build();
}

Bytes non_resident_data_record(std::vector<DataRunSpec> runs, std::uint64_t real_size) {
    return FileRecordBuilder{}
        .file_name(u"nonresident.bin")
        .non_resident_data(std::move(runs), real_size)
        .build();
}

Bytes truncated_record(std::size_t byte_count) {
    auto bytes = valid_file_record();
    bytes.resize(std::min(byte_count, bytes.size()));
    return bytes;
}

Bytes malformed_first_attribute_offset(std::uint16_t offset) {
    auto bytes = valid_file_record();
    put_le<std::uint16_t>(bytes, 0x14, offset);
    return bytes;
}

Bytes malformed_attribute_size(std::uint32_t size) {
    auto bytes = valid_file_record();
    put_le<std::uint32_t>(bytes, FirstAttributeOffset + 4U, size);
    return bytes;
}

Bytes malformed_resident_content(std::uint16_t offset, std::uint32_t size) {
    auto bytes = resident_data_record({0x10, 0x20, 0x30});
    const auto attribute = find_attribute(bytes, AttributeData);
    put_le<std::uint32_t>(bytes, attribute + 16U, size);
    put_le<std::uint16_t>(bytes, attribute + 20U, offset);
    return bytes;
}

Bytes malformed_non_resident_run_offset(std::uint16_t offset) {
    auto bytes = non_resident_data_record({positive_run(1, 7)}, 4096);
    const auto attribute = find_attribute(bytes, AttributeData);
    put_le<std::uint16_t>(bytes, attribute + 32U, offset);
    return bytes;
}

Bytes invalid_utf16_record(std::vector<std::uint16_t> code_units) {
    std::u16string name;
    name.reserve(code_units.size());
    for (const auto code_unit : code_units) {
        name.push_back(static_cast<char16_t>(code_unit));
    }
    return valid_file_record(std::move(name));
}

Bytes overflowing_data_runs_record() {
    return non_resident_data_record(
        {positive_run(1, std::numeric_limits<std::int64_t>::max()), positive_run(1, 1)},
        8192);
}

Bytes corrupt_data_runs_record() {
    auto bytes = non_resident_data_record({positive_run(1, 7)}, 4096);
    const auto attribute = find_attribute(bytes, AttributeData);
    const auto run_offset = get_le<std::uint16_t>(bytes, attribute + 32U);
    bytes[attribute + run_offset] = 0x99; // Both field widths exceed the NTFS limit of eight.
    return bytes;
}

SanitizedNameCollision sanitized_name_collision() {
    return {valid_file_record(u"reports/2026"),
            valid_file_record(u"reports\\2026"),
            "reports_2026"};
}

} // namespace ntfs::test
