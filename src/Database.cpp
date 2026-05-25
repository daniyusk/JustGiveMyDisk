#include "Database.hpp"

#include <fmt/core.h>

#include <stdexcept>

namespace {

std::uint64_t column_u64(sqlite3_stmt* stmt, int column) {
    return static_cast<std::uint64_t>(sqlite3_column_int64(stmt, column));
}

StoredRecord row_to_record(sqlite3_stmt* stmt) {
    StoredRecord record;
    record.id = sqlite3_column_int64(stmt, 0);
    record.record_id_guess = column_u64(stmt, 1);
    record.offset = column_u64(stmt, 2);
    record.parent_ref = column_u64(stmt, 3);
    const auto* text = sqlite3_column_text(stmt, 4);
    record.name = text ? reinterpret_cast<const char*>(text) : "";
    record.name_namespace = sqlite3_column_int(stmt, 5);
    record.flags = static_cast<std::uint32_t>(sqlite3_column_int64(stmt, 6));
    record.allocated_size = column_u64(stmt, 7);
    record.real_size = column_u64(stmt, 8);
    record.is_directory = sqlite3_column_int(stmt, 9) != 0;
    return record;
}

void check_sqlite(int rc, sqlite3* db, const char* action) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        throw std::runtime_error(fmt::format("{}: {}", action, sqlite3_errmsg(db)));
    }
}

} // namespace

Database::Database(const std::string& path) {
    const int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "unknown sqlite error";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        throw std::runtime_error(fmt::format("failed to open database {}: {}", path, message));
    }
}

Database::~Database() {
    if (insert_stmt_) {
        sqlite3_finalize(insert_stmt_);
    }
    if (db_) {
        sqlite3_close(db_);
    }
}

void Database::initialize() {
    exec("PRAGMA journal_mode=WAL;");
    exec("CREATE TABLE IF NOT EXISTS records("
         "id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "record_id_guess INTEGER,"
         "offset INTEGER NOT NULL,"
         "parent_ref INTEGER,"
         "name TEXT,"
         "namespace INTEGER,"
         "flags INTEGER,"
         "allocated_size INTEGER,"
         "real_size INTEGER,"
         "is_directory INTEGER"
         ");");
    exec("CREATE INDEX IF NOT EXISTS idx_records_name ON records(name);");
    exec("CREATE INDEX IF NOT EXISTS idx_records_parent_ref ON records(parent_ref);");
    exec("CREATE INDEX IF NOT EXISTS idx_records_record_id_guess ON records(record_id_guess);");
}

void Database::begin() {
    exec("BEGIN IMMEDIATE;");
}

void Database::commit() {
    exec("COMMIT;");
}

void Database::insert_record(const MftRecord& record, const MftFileName& name) {
    if (!insert_stmt_) {
        constexpr const char* sql =
            "INSERT INTO records("
            "record_id_guess, offset, parent_ref, name, namespace, flags, "
            "allocated_size, real_size, is_directory"
            ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
        check_sqlite(sqlite3_prepare_v2(db_, sql, -1, &insert_stmt_, nullptr), db_, "prepare insert");
    }

    sqlite3_reset(insert_stmt_);
    sqlite3_clear_bindings(insert_stmt_);
    sqlite3_bind_int64(insert_stmt_, 1, static_cast<sqlite3_int64>(record.record_id_guess));
    sqlite3_bind_int64(insert_stmt_, 2, static_cast<sqlite3_int64>(record.offset));
    sqlite3_bind_int64(insert_stmt_, 3, static_cast<sqlite3_int64>(name.parent_ref));
    sqlite3_bind_text(insert_stmt_, 4, name.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(insert_stmt_, 5, name.name_namespace);
    sqlite3_bind_int64(insert_stmt_, 6, static_cast<sqlite3_int64>(name.flags));
    sqlite3_bind_int64(insert_stmt_, 7, static_cast<sqlite3_int64>(name.allocated_size));
    sqlite3_bind_int64(insert_stmt_, 8, static_cast<sqlite3_int64>(name.real_size));
    sqlite3_bind_int(insert_stmt_, 9, name.is_directory ? 1 : 0);

    check_sqlite(sqlite3_step(insert_stmt_), db_, "insert record");
}

std::vector<StoredRecord> Database::find_by_name(const std::string& name) const {
    constexpr const char* sql =
        "SELECT id, record_id_guess, offset, parent_ref, name, namespace, flags, "
        "allocated_size, real_size, is_directory "
        "FROM records WHERE name = ? ORDER BY id;";
    sqlite3_stmt* stmt = nullptr;
    check_sqlite(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), db_, "prepare find");
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<StoredRecord> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        rows.push_back(row_to_record(stmt));
    }
    sqlite3_finalize(stmt);
    return rows;
}

std::optional<StoredRecord> Database::get_by_id(std::int64_t id) const {
    constexpr const char* sql =
        "SELECT id, record_id_guess, offset, parent_ref, name, namespace, flags, "
        "allocated_size, real_size, is_directory "
        "FROM records WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    check_sqlite(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), db_, "prepare get");
    sqlite3_bind_int64(stmt, 1, id);

    std::optional<StoredRecord> row;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        row = row_to_record(stmt);
    }
    sqlite3_finalize(stmt);
    return row;
}

std::vector<StoredRecord> Database::children_of(std::uint64_t parent_ref) const {
    constexpr const char* sql =
        "SELECT id, record_id_guess, offset, parent_ref, name, namespace, flags, "
        "allocated_size, real_size, is_directory "
        "FROM records WHERE parent_ref = ? ORDER BY is_directory DESC, name COLLATE NOCASE, id;";
    sqlite3_stmt* stmt = nullptr;
    check_sqlite(sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr), db_, "prepare children");
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(parent_ref));

    std::vector<StoredRecord> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        rows.push_back(row_to_record(stmt));
    }
    sqlite3_finalize(stmt);
    return rows;
}

void Database::exec(const char* sql) const {
    char* error = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::string message = error ? error : sqlite3_errmsg(db_);
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}
