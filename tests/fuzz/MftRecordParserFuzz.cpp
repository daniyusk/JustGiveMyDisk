#include "MftRecord.hpp"
#include "NtfsFixtures.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

volatile std::size_t observed_result = 0;

void exercise_parser(const std::vector<std::uint8_t>& bytes) {
    MftRecordParser parser;
    std::size_t observed = 0;

    if (const auto record = parser.parse(bytes, 0)) {
        observed += record->names.size();
    }
    if (const auto number = parser.record_number(bytes)) {
        observed ^= static_cast<std::size_t>(*number);
    }
    for (const auto& attribute : parser.data_attributes(bytes)) {
        observed += attribute.resident_data.size();
        observed += attribute.runs.size();
    }

    auto restored = bytes;
    observed += MftRecordParser::restore_usa(restored) ? 1U : 0U;
    observed_result = observed;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    constexpr std::size_t MaximumInputSize = 1U << 20U;
    if (size > MaximumInputSize) {
        return 0;
    }

    std::vector<std::uint8_t> raw;
    if (size != 0) {
        raw.assign(data, data + size);
    }
    exercise_parser(raw);

    // A deterministic valid skeleton lets even a tiny corpus mutate deep fields.
    // Each three-byte group selects a record offset and its replacement byte.
    auto structured = ntfs::test::resident_data_record({0x10, 0x20, 0x30, 0x40});
    for (std::size_t i = 0; i + 2U < size; i += 3U) {
        const std::size_t offset =
            ((static_cast<std::size_t>(data[i]) << 8U) | data[i + 1U]) % structured.size();
        structured[offset] = data[i + 2U];
    }
    exercise_parser(structured);

    return 0;
}
