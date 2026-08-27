#include "Recover.hpp"

#include "Database.hpp"
#include "MftRecord.hpp"
#include "PathSafety.hpp"
#include "RecoveryOutput.hpp"

#include <fmt/core.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

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
                  std::unordered_set<std::uint64_t>& visited_dirs,
                  recovery_output::CollisionTracker& collisions) {
    if (!visited_dirs.insert(parent_record_id).second) {
        return;
    }

    for (const auto& child : dedup_children(db.children_of(parent_record_id))) {
        const auto relative = base / recovery_output::sanitize_name(child.name);
        collisions.add(relative, child.name, child.record_id_guess);
        items.push_back({child, relative});
        if (child.is_directory) {
            collect_tree(db, child.record_id_guess, relative, items, visited_dirs, collisions);
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

bool write_zeros(recovery_output::AtomicFile& out, std::uint64_t size) {
    std::vector<char> zeros(1024 * 1024, 0);
    while (size > 0) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(size, zeros.size()));
        out.write(zeros.data(), chunk);
        size -= chunk;
    }
    return true;
}

bool copy_nonresident(int fd,
                      const DataAttribute& data,
                      std::uint64_t cluster_size,
                      std::uint64_t bytes_to_copy,
                      recovery_output::AtomicFile& out) {
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
            out.write(buffer.data(), chunk);
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
                  const std::filesystem::path& path,
                  bool overwrite) {
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

    recovery_output::AtomicFile out(path, overwrite);

    if (data.resident) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(bytes_to_copy, data.resident_data.size()));
        out.write(data.resident_data.data(), count);
        if (count != bytes_to_copy) {
            return false;
        }
        out.commit();
        return true;
    }

    if (!copy_nonresident(fd, data, cluster_size, bytes_to_copy, out)) {
        return false;
    }
    out.commit();
    return true;
}

void ensure_safe_directory(const path_safety::Source& source, const std::filesystem::path& directory) {
    std::filesystem::path current = directory.root_path();
    for (const auto& component : directory.relative_path()) {
        current /= component;
        struct stat info {};
        if (::lstat(current.c_str(), &info) != 0) {
            if (errno != ENOENT) {
                throw std::runtime_error(fmt::format(
                    "cannot inspect output directory '{}': {}", current.string(), std::strerror(errno)));
            }
            if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                throw std::runtime_error(fmt::format(
                    "cannot create output directory '{}': {}", current.string(), std::strerror(errno)));
            }
            if (::lstat(current.c_str(), &info) != 0) {
                throw std::runtime_error(fmt::format(
                    "cannot inspect created output directory '{}': {}", current.string(), std::strerror(errno)));
            }
        }
        if (S_ISLNK(info.st_mode)) {
            throw std::runtime_error(fmt::format(
                "refusing symlink in output directory path '{}'", current.string()));
        }
        if (!S_ISDIR(info.st_mode)) {
            throw std::runtime_error(fmt::format(
                "output directory component '{}' is not a directory", current.string()));
        }
        path_safety::validate_destination_path(source, current);
    }
}

} // namespace

RecoverStats Recover::run(const RecoverOptions& options) {
    return run(options, {});
}

RecoverStats Recover::run(const RecoverOptions& options, const LogCallback& log) {
    const auto emit = [&](const std::string& message) {
        if (log) {
            log(message);
        } else {
            fmt::print("{}\n", message);
        }
    };

    path_safety::Source source(options.source);
    const auto database_path = path_safety::validate_database_path(source, options.database_path);
    const auto destination = path_safety::validate_destination_path(source, options.destination);
    Database db(database_path.string());
    const std::uint64_t cluster_size = read_cluster_size(source.fd());

    const auto root = db.get_by_record_id(options.root_record_id);
    if (!root) {
        throw std::runtime_error(fmt::format("no indexed record with record_id_guess {}", options.root_record_id));
    }

    std::vector<RecoverItem> items;
    std::unordered_set<std::uint64_t> visited_dirs;
    recovery_output::CollisionTracker collisions;
    collect_tree(db, options.root_record_id, {}, items, visited_dirs, collisions);

    RecoverStats stats;
    stats.preview_items = items.size();
    if (options.dry_run) {
        emit(fmt::format("dry-run: root record {} -> {}", options.root_record_id, destination.string()));
        for (const auto& item : items) {
            emit(fmt::format("{} {}",
                             item.record.is_directory ? "dir " : "file",
                             (destination / item.relative_path).string()));
        }
        emit(fmt::format("dry-run: {} item(s)", items.size()));
        return stats;
    }

    ensure_safe_directory(source, destination);

    std::unordered_set<std::uint64_t> wanted_files;
    for (const auto& item : items) {
        if (!item.record.is_directory) {
            wanted_files.insert(item.record.record_id_guess);
        }
    }

    emit(fmt::format("Locating {} file MFT record(s) by FILE header record number", wanted_files.size()));
    const auto records = locate_records(source.fd(), wanted_files);

    for (const auto& item : items) {
        const auto target = destination / item.relative_path;
        try {
            if (item.record.is_directory) {
                ensure_safe_directory(source, target);
                continue;
            }

            ensure_safe_directory(source, target.parent_path());
            path_safety::validate_destination_path(source, target.parent_path());

            const auto found = records.find(item.record.record_id_guess);
            if (found == records.end() ||
                !recover_file(source.fd(), item.record, found->second, cluster_size, target, options.overwrite)) {
                ++stats.skipped;
                emit(fmt::format("skipped {}", target.string()));
                continue;
            }
            ++stats.recovered;
            emit(fmt::format("recovered {}", target.string()));
        } catch (const std::exception& e) {
            ++stats.skipped;
            emit(fmt::format("skipped {} ({})", target.string(), e.what()));
        }
    }

    emit(fmt::format("done: recovered={} skipped={}", stats.recovered, stats.skipped));
    return stats;
}
