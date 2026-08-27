#include "MftRecord.hpp"
#include "NtfsFixtures.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            throw std::runtime_error(std::string{"check failed: "} + #condition + \
                                     " at line " + std::to_string(__LINE__)); \
        } \
    } while (false)

using Test = std::pair<const char*, void (*)()>;

void valid_file_record_is_parsed() {
    MftRecordParser parser;
    auto bytes = ntfs::test::FileRecordBuilder{}
        .record_number(77)
        .file_name(u"backup\U0001F4BE.bin", 5, 1)
        .build();

    const auto record = parser.parse(bytes, 2048);
    CHECK(record.has_value());
    CHECK(record->record_id_guess == 77);
    CHECK(record->offset == 2048);
    CHECK(record->names.size() == 1);
    CHECK(record->names[0].parent_ref == 5);
    CHECK(record->names[0].name == "backup\xF0\x9F\x92\xBE.bin");
}

void usa_valid_and_invalid_are_distinguished() {
    MftRecordParser parser;
    auto valid = ntfs::test::valid_usa_record();
    CHECK(MftRecordParser::restore_usa(valid));
    CHECK(!parser.parse(ntfs::test::invalid_usa_record(), 0).has_value());
}

void resident_data_is_parsed() {
    MftRecordParser parser;
    const std::vector<std::uint8_t> payload{0x00, 0x7F, 0x80, 0xFF};
    const auto attributes = parser.data_attributes(ntfs::test::resident_data_record(payload));

    CHECK(attributes.size() == 1);
    CHECK(attributes[0].resident);
    CHECK(attributes[0].real_size == payload.size());
    CHECK(attributes[0].resident_data == payload);
}

void positive_negative_and_sparse_runs_are_parsed() {
    MftRecordParser parser;
    const auto bytes = ntfs::test::non_resident_data_record(
        {ntfs::test::positive_run(3, 10),
         ntfs::test::positive_run(2, 5),
         ntfs::test::negative_run(4, -3),
         ntfs::test::sparse_run(6)},
        15 * 4096);
    const auto attributes = parser.data_attributes(bytes);

    CHECK(attributes.size() == 1);
    CHECK(!attributes[0].resident);
    CHECK(attributes[0].runs.size() == 4);
    CHECK(attributes[0].runs[0].lcn == 10);
    CHECK(attributes[0].runs[1].lcn == 15);
    CHECK(attributes[0].runs[2].lcn == 12);
    CHECK(attributes[0].runs[3].sparse);
    CHECK(attributes[0].runs[3].cluster_count == 6);
}

void truncated_records_are_rejected() {
    MftRecordParser parser;
    for (const std::size_t size : {std::size_t{0}, std::size_t{3}, std::size_t{48},
                                   std::size_t{511}, std::size_t{1023}}) {
        const auto bytes = ntfs::test::truncated_record(size);
        CHECK(!parser.parse(bytes, 0).has_value());
        CHECK(!parser.record_number(bytes).has_value());
        CHECK(parser.data_attributes(bytes).empty());
    }
}

void malformed_offsets_and_sizes_stay_in_bounds() {
    MftRecordParser parser;
    CHECK(!parser.parse(ntfs::test::malformed_first_attribute_offset(0xFFFF), 0).has_value());
    CHECK(!parser.parse(ntfs::test::malformed_attribute_size(0xFFFFFFFF), 0).has_value());
    CHECK(parser.data_attributes(
        ntfs::test::malformed_resident_content(0xFFFF, 0xFFFFFFFF)).empty());
    CHECK(parser.data_attributes(
        ntfs::test::malformed_non_resident_run_offset(0xFFFF)).empty());
}

void invalid_surrogate_pairs_are_replaced() {
    MftRecordParser parser;
    const auto high_then_ascii = parser.parse(
        ntfs::test::invalid_utf16_record({0xD83D, 0x0041}), 0);
    const auto trailing_high = parser.parse(
        ntfs::test::invalid_utf16_record({0x0041, 0xD83D}), 0);
    const auto lone_low = parser.parse(
        ntfs::test::invalid_utf16_record({0xDC00}), 0);

    CHECK(high_then_ascii.has_value());
    CHECK(high_then_ascii->names[0].name == "\xEF\xBF\xBD" "A");
    CHECK(trailing_high.has_value());
    CHECK(trailing_high->names[0].name == "A" "\xEF\xBF\xBD");
    CHECK(lone_low.has_value());
    CHECK(lone_low->names[0].name == "\xEF\xBF\xBD");
}

void sanitized_name_collisions_are_explicit_fixtures() {
    MftRecordParser parser;
    const auto collision = ntfs::test::sanitized_name_collision();
    const auto first = parser.parse(collision.first, 0);
    const auto second = parser.parse(collision.second, 1024);

    CHECK(first.has_value());
    CHECK(second.has_value());
    CHECK(first->names[0].name == "reports/2026");
    CHECK(second->names[0].name == "reports\\2026");
    CHECK(first->names[0].name != second->names[0].name);
    CHECK(collision.sanitized_name == "reports_2026");
}

void data_run_overflow_is_rejected() {
    MftRecordParser parser;
    CHECK(parser.data_attributes(ntfs::test::overflowing_data_runs_record()).empty());
}

void corrupt_records_are_rejected_without_partial_data() {
    MftRecordParser parser;
    CHECK(parser.data_attributes(ntfs::test::corrupt_data_runs_record()).empty());
    CHECK(!parser.parse(ntfs::test::invalid_usa_record(), 0).has_value());
}

} // namespace

int main() {
    const std::vector<Test> tests{
        {"valid FILE record", valid_file_record_is_parsed},
        {"USA validation", usa_valid_and_invalid_are_distinguished},
        {"resident attribute", resident_data_is_parsed},
        {"non-resident data runs", positive_negative_and_sparse_runs_are_parsed},
        {"truncated records", truncated_records_are_rejected},
        {"malformed offsets and sizes", malformed_offsets_and_sizes_stay_in_bounds},
        {"invalid UTF-16", invalid_surrogate_pairs_are_replaced},
        {"sanitized name collision", sanitized_name_collisions_are_explicit_fixtures},
        {"data run overflow", data_run_overflow_is_rejected},
        {"corruption regression", corrupt_records_are_rejected_without_partial_data},
    };

    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "ok - " << name << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "not ok - " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
