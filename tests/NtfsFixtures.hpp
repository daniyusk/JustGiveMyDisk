#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ntfs::test {

using Bytes = std::vector<std::uint8_t>;

struct DataRunSpec {
    std::uint64_t cluster_count = 0;
    std::optional<std::int64_t> lcn_delta;
};

DataRunSpec positive_run(std::uint64_t cluster_count, std::int64_t lcn_delta);
DataRunSpec negative_run(std::uint64_t cluster_count, std::int64_t lcn_delta);
DataRunSpec sparse_run(std::uint64_t cluster_count);

class FileRecordBuilder {
public:
    FileRecordBuilder();

    FileRecordBuilder& record_number(std::uint32_t value);
    FileRecordBuilder& flags(std::uint16_t value);
    FileRecordBuilder& file_name(std::u16string code_units,
                                 std::uint64_t parent_ref = 5,
                                 std::uint8_t name_namespace = 1);
    FileRecordBuilder& resident_data(Bytes data);
    FileRecordBuilder& non_resident_data(std::vector<DataRunSpec> runs,
                                         std::uint64_t real_size);
    FileRecordBuilder& invalid_usa();

    Bytes build() const;

private:
    void append_resident_attribute(std::uint32_t type, const Bytes& content);

    Bytes bytes_;
    std::size_t attribute_offset_ = 0;
    bool invalid_usa_ = false;
};

Bytes valid_file_record(std::u16string name = u"fixture.txt");
Bytes valid_usa_record();
Bytes invalid_usa_record();
Bytes resident_data_record(Bytes data);
Bytes non_resident_data_record(std::vector<DataRunSpec> runs,
                               std::uint64_t real_size);
Bytes truncated_record(std::size_t byte_count);
Bytes malformed_first_attribute_offset(std::uint16_t offset);
Bytes malformed_attribute_size(std::uint32_t size);
Bytes malformed_resident_content(std::uint16_t offset, std::uint32_t size);
Bytes malformed_non_resident_run_offset(std::uint16_t offset);
Bytes invalid_utf16_record(std::vector<std::uint16_t> code_units);
Bytes overflowing_data_runs_record();
Bytes corrupt_data_runs_record();

struct SanitizedNameCollision {
    Bytes first;
    Bytes second;
    std::string sanitized_name;
};

SanitizedNameCollision sanitized_name_collision();

} // namespace ntfs::test
