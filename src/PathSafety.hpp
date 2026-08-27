#pragma once

#include <filesystem>
#include <cstdint>
#include <string>

namespace path_safety {

class Source {
public:
    explicit Source(const std::filesystem::path& path);
    ~Source();

    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;

    Source(Source&& other) noexcept;
    Source& operator=(Source&& other) noexcept;

    int fd() const noexcept;
    const std::filesystem::path& canonical_path() const noexcept;
    bool is_block_device() const noexcept;
    std::uint64_t block_device() const noexcept;

private:
    int fd_ = -1;
    std::filesystem::path canonical_path_;
    std::uint64_t block_device_ = 0;
    bool is_block_device_ = false;
};

// These checks are deliberately performed before SQLite or recovery output is
// opened. Both the CLI and TUI reach them through Scanner and Recover.
std::filesystem::path validate_database_path(
    const Source& source, const std::filesystem::path& database_path);
std::filesystem::path validate_destination_path(
    const Source& source, const std::filesystem::path& destination_path);

} // namespace path_safety
