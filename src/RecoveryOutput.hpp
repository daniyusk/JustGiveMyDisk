#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace recovery_output {

std::string sanitize_name(std::string name);

class CollisionTracker {
public:
    void add(const std::filesystem::path& relative_path,
             const std::string& original_name,
             std::uint64_t record_id);

private:
    struct Entry {
        std::string original_name;
        std::uint64_t record_id = 0;
        std::filesystem::path path;
    };
    std::unordered_map<std::string, Entry> entries_;
};

class AtomicFile {
public:
    AtomicFile(const std::filesystem::path& final_path, bool overwrite);
    ~AtomicFile();

    AtomicFile(const AtomicFile&) = delete;
    AtomicFile& operator=(const AtomicFile&) = delete;

    void write(const void* data, std::size_t size);
    void commit();

    const std::filesystem::path& partial_path() const noexcept;

private:
    std::filesystem::path final_path_;
    std::filesystem::path partial_path_;
    int fd_ = -1;
    bool overwrite_ = false;
    bool committed_ = false;

    void cleanup_partial() noexcept;
};

} // namespace recovery_output
