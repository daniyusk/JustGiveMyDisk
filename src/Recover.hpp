#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct RecoverOptions {
    std::string source;
    std::string database_path;
    std::uint64_t root_record_id = 0;
    std::string destination;
    bool dry_run = false;
};

struct RecoverStats {
    std::uint64_t recovered = 0;
    std::uint64_t skipped = 0;
    std::uint64_t preview_items = 0;
};

class Recover {
public:
    using LogCallback = std::function<void(const std::string&)>;

    RecoverStats run(const RecoverOptions& options);
    RecoverStats run(const RecoverOptions& options, const LogCallback& log);
};
