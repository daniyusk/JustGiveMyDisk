#pragma once

#include "Database.hpp"
#include "MftRecord.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

struct ScanOptions {
    std::string source;
    std::string database_path;
    std::optional<std::string> target;
};

struct ScanStats {
    std::uint64_t bytes_scanned = 0;
    std::uint64_t candidate_file_signatures = 0;
    std::uint64_t valid_records = 0;
    std::uint64_t inserted_names = 0;
    std::uint64_t target_matches = 0;
};

struct ScanProgress {
    ScanStats stats;
    std::optional<std::uint64_t> total_size;
    std::string message;
};

class Scanner {
public:
    using ProgressCallback = std::function<void(const ScanProgress&)>;

    ScanStats run(const ScanOptions& options);
    ScanStats run(const ScanOptions& options, const ProgressCallback& progress);
};
