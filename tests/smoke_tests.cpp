#include "Database.hpp"
#include "MftRecord.hpp"
#include "Utf.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "check failed: " << message << '\n';
    }
    return condition;
}

bool utf_smoke() {
    const std::vector<std::uint8_t> input{
        0x41, 0x00,
        0x3D, 0xD8, 0x00, 0xDE,
        0x00, 0xDC,
    };
    const std::string expected = "A\xF0\x9F\x98\x80\xEF\xBF\xBD";
    return check(utf16le_to_utf8(input.data(), input.size()) == expected,
                 "UTF-16LE conversion should handle surrogate pairs and invalid low surrogates");
}

bool parser_smoke() {
    MftRecordParser parser;
    std::vector<std::uint8_t> bytes(MftRecordParser::RecordSize, 0);

    if (!check(!parser.parse(bytes, 0).has_value(), "zeroed input must not parse as an MFT record")) {
        return false;
    }

    bytes[0] = 'F';
    bytes[1] = 'I';
    bytes[2] = 'L';
    bytes[3] = 'E';
    return check(!parser.parse(bytes, 0).has_value(), "an invalid update sequence array must be rejected");
}

bool database_smoke() {
    Database database(":memory:");
    database.initialize();

    MftRecord record;
    record.record_id_guess = 42;
    record.offset = MftRecordParser::RecordSize;

    MftFileName name;
    name.parent_ref = 5;
    name.name = "smoke.txt";
    name.real_size = 12;

    database.begin();
    database.insert_record(record, name);
    database.commit();

    const auto matches = database.find_by_name(name.name);
    if (!check(matches.size() == 1, "inserted filename should be queryable")) {
        return false;
    }
    if (!check(matches.front().record_id_guess == record.record_id_guess,
               "record id should survive a database round trip")) {
        return false;
    }

    const auto stored = database.get_by_record_id(record.record_id_guess);
    if (!check(stored.has_value(), "record lookup by MFT id should succeed")) {
        return false;
    }
    return check(database.children_of(name.parent_ref).size() == 1,
                 "record should be queryable through its parent reference");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: jgmd_smoke_tests <utf|parser|database>\n";
        return 2;
    }

    const std::string_view test = argv[1];
    if (test == "utf") {
        return utf_smoke() ? 0 : 1;
    }
    if (test == "parser") {
        return parser_smoke() ? 0 : 1;
    }
    if (test == "database") {
        return database_smoke() ? 0 : 1;
    }

    std::cerr << "unknown smoke test: " << test << '\n';
    return 2;
}
