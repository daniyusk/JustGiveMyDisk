#include "Scanner.hpp"

#include "PathSafety.hpp"

#include <fmt/core.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {

std::optional<std::uint64_t> source_size(int fd) {
    struct stat st {};
    if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        return static_cast<std::uint64_t>(st.st_size);
    }
    const off_t end = ::lseek(fd, 0, SEEK_END);
    if (end >= 0) {
        ::lseek(fd, 0, SEEK_SET);
        return static_cast<std::uint64_t>(end);
    }
    return std::nullopt;
}

void print_record_match(const char* prefix, const MftRecord& record, const MftFileName& name) {
    fmt::print("{} id_guess={} db_parent={} offset={} name=\"{}\" ns={} dir={} alloc={} real={} flags=0x{:x}\n",
               prefix,
               record.record_id_guess,
               name.parent_ref,
               record.offset,
               name.name,
               name.name_namespace,
               name.is_directory ? 1 : 0,
               name.allocated_size,
               name.real_size,
               name.flags);
}

} // namespace

ScanStats Scanner::run(const ScanOptions& options) {
    return run(options, {});
}

ScanStats Scanner::run(const ScanOptions& options, const ProgressCallback& progress) {
    path_safety::Source source(options.source);
    const auto database_path = path_safety::validate_database_path(source, options.database_path);
    Database database(database_path.string());
    database.initialize();

    ScanStats stats;
    const auto total_size = source_size(source.fd());
    if (total_size) {
        const auto message = fmt::format("Scanning {} bytes from {} in read-only mode", *total_size, options.source);
        if (progress) {
            progress(ScanProgress{stats, total_size, message});
        } else {
            fmt::print("{}\n", message);
        }
    } else {
        const auto message = fmt::format("Scanning {} in read-only mode", options.source);
        if (progress) {
            progress(ScanProgress{stats, total_size, message});
        } else {
            fmt::print("{}\n", message);
        }
    }

    MftRecordParser parser;
    std::vector<std::uint8_t> buffer(MftRecordParser::RecordSize);

    database.begin();
    std::uint64_t offset = 0;
    std::uint64_t next_progress = 0;

    while (true) {
        std::size_t filled = 0;
        while (filled < buffer.size()) {
            const ssize_t n = ::read(source.fd(), buffer.data() + filled, buffer.size() - filled);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                database.commit();
                throw std::runtime_error(fmt::format("read failed at offset {}: {}", offset + filled, std::strerror(errno)));
            }
            if (n == 0) {
                break;
            }
            filled += static_cast<std::size_t>(n);
        }

        if (filled < buffer.size()) {
            break;
        }

        if (std::memcmp(buffer.data(), "FILE", 4) == 0) {
            ++stats.candidate_file_signatures;
            if (auto record = parser.parse(buffer, offset)) {
                ++stats.valid_records;
                for (const auto& name : record->names) {
                    database.insert_record(*record, name);
                    ++stats.inserted_names;

                    if (options.target && name.name == *options.target) {
                        ++stats.target_matches;
                        print_record_match("match", *record, name);
                    }
                }
            }
        }

        offset += buffer.size();
        stats.bytes_scanned = offset;

        if (offset >= next_progress) {
            if (total_size && *total_size > 0) {
                const double pct = (static_cast<double>(offset) / static_cast<double>(*total_size)) * 100.0;
                const auto message = fmt::format("progress: {} bytes ({:.2f}%), valid_records={}, names={}",
                                                 offset, pct, stats.valid_records, stats.inserted_names);
                if (progress) {
                    progress(ScanProgress{stats, total_size, message});
                } else {
                    fmt::print("{}\n", message);
                }
            } else {
                const auto message = fmt::format("progress: {} bytes, valid_records={}, names={}",
                                                 offset, stats.valid_records, stats.inserted_names);
                if (progress) {
                    progress(ScanProgress{stats, total_size, message});
                } else {
                    fmt::print("{}\n", message);
                }
            }
            next_progress = offset + (256ULL * 1024ULL * 1024ULL);
        }
    }

    database.commit();
    const auto message = fmt::format("done: scanned={} candidates={} valid_records={} names={} target_matches={}",
                                     stats.bytes_scanned,
                                     stats.candidate_file_signatures,
                                     stats.valid_records,
                                     stats.inserted_names,
                                     stats.target_matches);
    if (progress) {
        progress(ScanProgress{stats, total_size, message});
    } else {
        fmt::print("{}\n", message);
    }
    return stats;
}
