#include "Recover.hpp"

#include "Database.hpp"
#include "MftRecord.hpp"

#include <fmt/core.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

class SourceDevice {
public:
    explicit SourceDevice(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) {
            throw std::runtime_error(fmt::format("failed to open source read-only {}: {}", path, std::strerror(errno)));
        }
    }

    ~SourceDevice() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    SourceDevice(const SourceDevice&) = delete;
    SourceDevice& operator=(const SourceDevice&) = delete;

    int get() const {
        return fd_;
    }

private:
    int fd_ = -1;
};

struct RecoverItem {
    StoredRecord record;
    std::filesystem::path relative_path;
};

std::uint16_t le16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0]) |
        (static_cast<std::uint16_t>(data[1]) << 8U);
}

std::uint64_t checked_mul(std::uint64_t lhs, std::uint64_t rhs, const char* label) {
    if (rhs != 0 && lhs > UINT64_MAX / rhs) {
        throw std::runtime_error(fmt::format("{} overflow", label));
    }
    return lhs * rhs;
}

std::uint64_t read_cluster_size(int fd) {
    std::uint8_t boot[512] {};
    const ssize_t n = ::pread(fd, boot, sizeof(boot), 0);
    if (n != static_cast<ssize_t>(sizeof(boot))) {
        throw std::runtime_error(fmt::format("failed to read NTFS boot sector: {}", std::strerror(errno)));
    }

    const std::uint16_t bytes_per_sector = le16(boot + 0x0B);
    const std::uint8_t sectors_per_cluster = boot[0x0D];
    if (bytes_per_sector == 0 || sectors_per_cluster == 0) {
        throw std::runtime_error("invalid NTFS boot sector cluster geometry");
    }
    return checked_mul(bytes_per_sector, sectors_per_cluster, "cluster size");
}

std::string safe_name(std::string name) {
    for (char& ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (ch == '/' || ch == '\\' || c < 0x20) {
            ch = '_';
        }
    }
    if (name.empty() || name == "." || name == "..") {
        return "_";
    }
    return name;
}

bool better_name(const StoredRecord& candidate, const StoredRecord& current) {
    const bool candidate_dos = candidate.name_namespace == 2;
    const bool current_dos = current.name_namespace == 2;
    if (candidate_dos != current_dos) {
        return !candidate_dos;
    }
    if (candidate.name.size() != current.name.size()) {
        return candidate.name.size() > current.name.size();
    }
    return candidate.id < current.id;
}

std::vector<StoredRecord> dedup_children(const std::vector<StoredRecord>& children) {
    std::unordered_map<std::uint64_t, bool> record_has_long_name;
    for (const auto& child : children) {
        if (child.name_namespace != 2) {
            record_has_long_name[child.record_id_guess] = true;
        }
    }

    std::unordered_map<std::string, StoredRecord> by_record_and_name;
    for (const auto& child : children) {
        if (child.name_namespace == 2 && record_has_long_name[child.record_id_guess]) {
            continue;
        }

        const auto key = fmt::format("{}\n{}", child.record_id_guess, child.name);
        auto it = by_record_and_name.find(key);
        if (it == by_record_and_name.end() || better_name(child, it->second)) {
            by_record_and_name[key] = child;
        }
    }

    std::vector<StoredRecord> result;
    result.reserve(by_record_and_name.size());
    for (auto& [_, record] : by_record_and_name) {
        result.push_back(std::move(record));
    }
    std::sort(result.begin(), result.end(), [](const StoredRecord& lhs, const StoredRecord& rhs) {
        if (lhs.is_directory != rhs.is_directory) {
            return lhs.is_directory > rhs.is_directory;
        }
        return lhs.name < rhs.name;
    });
    return result;
}

void collect_tree(Database& db,
                  std::uint64_t parent_record_id,
                  const std::filesystem::path& base,
                  std::vector<RecoverItem>& items,
                  std::unordered_set<std::uint64_t>& visited_dirs) {
    if (!visited_dirs.insert(parent_record_id).second) {
        return;
    }

    for (const auto& child : dedup_children(db.children_of(parent_record_id))) {
        const auto relative = base / safe_name(child.name);
        items.push_back({child, relative});
        if (child.is_directory) {
            collect_tree(db, child.record_id_guess, relative, items, visited_dirs);
        }
    }
}

bool read_exact_at(int fd, void* data, std::size_t size, std::uint64_t offset) {
    auto* out = static_cast<std::uint8_t*>(data);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::pread(fd, out + done, size - done, static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> locate_records(
    int fd,
    const std::unordered_set<std::uint64_t>& wanted) {
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> found;
    if (wanted.empty()) {
        return found;
    }

    MftRecordParser parser;
    std::vector<std::uint8_t> buffer(MftRecordParser::RecordSize);
    std::uint64_t offset = 0;
    while (found.size() < wanted.size()) {
        if (!read_exact_at(fd, buffer.data(), buffer.size(), offset)) {
            break;
        }
        if (std::memcmp(buffer.data(), "FILE", 4) == 0) {
            const auto number = parser.record_number(buffer);
            if (number && wanted.contains(*number) && !found.contains(*number)) {
                found.emplace(*number, buffer);
            }
        }
        offset += buffer.size();
    }
    return found;
}

bool write_zeros(std::ofstream& out, std::uint64_t size) {
    std::vector<char> zeros(1024 * 1024, 0);
    while (size > 0) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(size, zeros.size()));
        out.write(zeros.data(), static_cast<std::streamsize>(chunk));
        if (!out) {
            return false;
        }
        size -= chunk;
    }
    return true;
}

bool copy_nonresident(int fd,
                      const DataAttribute& data,
                      std::uint64_t cluster_size,
                      std::uint64_t bytes_to_copy,
                      std::ofstream& out) {
    std::vector<char> buffer(1024 * 1024);
    std::uint64_t remaining = bytes_to_copy;

    for (const auto& run : data.runs) {
        if (remaining == 0) {
            return true;
        }
        const std::uint64_t run_bytes = checked_mul(run.cluster_count, cluster_size, "data run size");
        std::uint64_t to_copy = std::min(remaining, run_bytes);

        if (run.sparse) {
            if (!write_zeros(out, to_copy)) {
                return false;
            }
            remaining -= to_copy;
            continue;
        }

        std::uint64_t source_offset = checked_mul(run.lcn, cluster_size, "data run offset");
        while (to_copy > 0) {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(to_copy, buffer.size()));
            if (!read_exact_at(fd, buffer.data(), chunk, source_offset)) {
                return false;
            }
            out.write(buffer.data(), static_cast<std::streamsize>(chunk));
            if (!out) {
                return false;
            }
            source_offset += chunk;
            to_copy -= chunk;
            remaining -= chunk;
        }
    }

    return remaining == 0;
}

bool recover_file(int fd,
                  const StoredRecord& record,
                  const std::vector<std::uint8_t>& mft_bytes,
                  std::uint64_t cluster_size,
                  const std::filesystem::path& path) {
    MftRecordParser parser;
    const auto attributes = parser.data_attributes(mft_bytes);
    if (attributes.empty()) {
        return false;
    }

    const auto& data = attributes.front();
    const std::uint64_t bytes_to_copy = data.real_size;
    if (record.real_size != 0 && record.real_size < bytes_to_copy) {
        return false;
    }

    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }

    if (data.resident) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(bytes_to_copy, data.resident_data.size()));
        out.write(reinterpret_cast<const char*>(data.resident_data.data()), static_cast<std::streamsize>(count));
        return static_cast<bool>(out) && count == bytes_to_copy;
    }

    return copy_nonresident(fd, data, cluster_size, bytes_to_copy, out);
}

} // namespace

RecoverStats Recover::run(const RecoverOptions& options) {
    Database db(options.database_path);
    SourceDevice source(options.source);
    const std::uint64_t cluster_size = read_cluster_size(source.get());

    const auto root = db.get_by_record_id(options.root_record_id);
    if (!root) {
        throw std::runtime_error(fmt::format("no indexed record with record_id_guess {}", options.root_record_id));
    }

    std::vector<RecoverItem> items;
    std::unordered_set<std::uint64_t> visited_dirs;
    collect_tree(db, options.root_record_id, {}, items, visited_dirs);

    RecoverStats stats;
    if (options.dry_run) {
        fmt::print("dry-run: root record {} -> {}\n", options.root_record_id, options.destination);
        for (const auto& item : items) {
            fmt::print("{} {}\n",
                       item.record.is_directory ? "dir " : "file",
                       (std::filesystem::path(options.destination) / item.relative_path).string());
        }
        fmt::print("dry-run: {} item(s)\n", items.size());
        return stats;
    }

    std::filesystem::create_directories(options.destination);

    std::unordered_set<std::uint64_t> wanted_files;
    for (const auto& item : items) {
        if (!item.record.is_directory) {
            wanted_files.insert(item.record.record_id_guess);
        }
    }

    fmt::print("Locating {} file MFT record(s) by FILE header record number\n", wanted_files.size());
    const auto records = locate_records(source.get(), wanted_files);

    for (const auto& item : items) {
        const auto target = std::filesystem::path(options.destination) / item.relative_path;
        try {
            if (item.record.is_directory) {
                std::filesystem::create_directories(target);
                continue;
            }

            const auto found = records.find(item.record.record_id_guess);
            if (found == records.end() ||
                !recover_file(source.get(), item.record, found->second, cluster_size, target)) {
                ++stats.skipped;
                fmt::print("skipped {}\n", target.string());
                continue;
            }
            ++stats.recovered;
            fmt::print("recovered {}\n", target.string());
        } catch (const std::exception& e) {
            ++stats.skipped;
            fmt::print("skipped {} ({})\n", target.string(), e.what());
        }
    }

    fmt::print("done: recovered={} skipped={}\n", stats.recovered, stats.skipped);
    return stats;
}
