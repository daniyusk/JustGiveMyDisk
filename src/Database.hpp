#pragma once

#include "MftRecord.hpp"

#include <cstdint>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <vector>

struct StoredRecord {
    std::int64_t id = 0;
    std::uint64_t record_id_guess = 0;
    std::uint64_t offset = 0;
    std::uint64_t parent_ref = 0;
    std::string name;
    int name_namespace = 0;
    std::uint32_t flags = 0;
    std::uint64_t allocated_size = 0;
    std::uint64_t real_size = 0;
    bool is_directory = false;
};

class Database {
public:
    explicit Database(const std::string& path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void initialize();
    void begin();
    void commit();
    void insert_record(const MftRecord& record, const MftFileName& name);
    std::vector<StoredRecord> find_by_name(const std::string& name) const;
    std::optional<StoredRecord> get_by_id(std::int64_t id) const;
    std::optional<StoredRecord> get_by_record_id(std::uint64_t record_id) const;
    std::vector<StoredRecord> children_of(std::uint64_t parent_ref) const;

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* insert_stmt_ = nullptr;

    void exec(const char* sql) const;
};
