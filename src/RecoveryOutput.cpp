#include "RecoveryOutput.hpp"

#include <fmt/core.h>

#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

namespace recovery_output {
namespace {

std::string collision_key(const std::filesystem::path& path) {
    std::string key = path.lexically_normal().generic_string();
    for (char& character : key) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x80) {
            character = static_cast<char>(std::tolower(byte));
        }
    }
    return key;
}

bool path_exists(const std::filesystem::path& path, struct stat& info) {
    if (::lstat(path.c_str(), &info) == 0) {
        return true;
    }
    if (errno == ENOENT || errno == ENOTDIR) {
        return false;
    }
    throw std::runtime_error(fmt::format(
        "cannot inspect output '{}': {}", path.string(), std::strerror(errno)));
}

void reject_unsafe_existing_output(const std::filesystem::path& path, const struct stat& info) {
    if (S_ISLNK(info.st_mode)) {
        throw std::runtime_error(fmt::format("refusing to replace output symlink '{}'", path.string()));
    }
    if (!S_ISREG(info.st_mode)) {
        throw std::runtime_error(fmt::format("refusing to replace non-regular output '{}'", path.string()));
    }
}

int exclusive_output_flags() {
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    return flags;
}

int rename_without_replacement(const std::filesystem::path& from,
                               const std::filesystem::path& to) {
#ifdef __linux__
    const int result = static_cast<int>(::syscall(
        SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE));
    if (result == 0 || (errno != ENOSYS && errno != EINVAL)) {
        return result;
    }
#endif
    // link+unlink is the portable no-replace fallback. The final name becomes
    // visible atomically, and an existing final path is never overwritten.
    if (::link(from.c_str(), to.c_str()) != 0) {
        return -1;
    }
    if (::unlink(from.c_str()) != 0) {
        const int unlink_error = errno;
        ::unlink(to.c_str());
        errno = unlink_error;
        return -1;
    }
    return 0;
}

} // namespace

std::string sanitize_name(std::string name) {
    for (char& character : name) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '/' || character == '\\' || byte < 0x20) {
            character = '_';
        }
    }
    if (name.empty() || name == "." || name == "..") {
        return "_";
    }
    return name;
}

void CollisionTracker::add(const std::filesystem::path& relative_path,
                           const std::string& original_name,
                           std::uint64_t record_id) {
    const auto key = collision_key(relative_path);
    const Entry candidate{original_name, record_id, relative_path};
    const auto [position, inserted] = entries_.emplace(key, candidate);
    if (!inserted) {
        const auto& previous = position->second;
        throw std::runtime_error(fmt::format(
            "sanitized output collision: '{}' (record {}, original '{}') and '{}' "
            "(record {}, original '{}') resolve to the same output path",
            previous.path.string(),
            previous.record_id,
            previous.original_name,
            relative_path.string(),
            record_id,
            original_name));
    }
}

AtomicFile::AtomicFile(const std::filesystem::path& final_path, bool overwrite)
    : final_path_(final_path),
      partial_path_(final_path.string() + ".partial"),
      overwrite_(overwrite) {
    struct stat final_info {};
    if (path_exists(final_path_, final_info)) {
        reject_unsafe_existing_output(final_path_, final_info);
        if (!overwrite_) {
            throw std::runtime_error(fmt::format(
                "output '{}' already exists; use --overwrite to replace it", final_path_.string()));
        }
    }

    struct stat partial_info {};
    if (path_exists(partial_path_, partial_info)) {
        reject_unsafe_existing_output(partial_path_, partial_info);
        if (!overwrite_) {
            throw std::runtime_error(fmt::format(
                "stale partial output '{}' exists; remove it or use --overwrite",
                partial_path_.string()));
        }
        if (::unlink(partial_path_.c_str()) != 0) {
            throw std::runtime_error(fmt::format(
                "cannot remove stale partial output '{}': {}",
                partial_path_.string(),
                std::strerror(errno)));
        }
    }

    fd_ = ::open(partial_path_.c_str(), exclusive_output_flags(), 0600);
    if (fd_ < 0) {
        throw std::runtime_error(fmt::format(
            "cannot create partial output '{}': {}", partial_path_.string(), std::strerror(errno)));
    }
#ifndef O_CLOEXEC
    if (::fcntl(fd_, F_SETFD, FD_CLOEXEC) != 0) {
        const auto error = std::strerror(errno);
        cleanup_partial();
        throw std::runtime_error(fmt::format(
            "cannot set close-on-exec on partial output '{}': {}", partial_path_.string(), error));
    }
#endif
}

AtomicFile::~AtomicFile() {
    cleanup_partial();
}

void AtomicFile::write(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t written = 0;
    while (written < size) {
        const ssize_t count = ::write(fd_, bytes + written, size - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(fmt::format(
                "write failed for partial output '{}': {}", partial_path_.string(), std::strerror(errno)));
        }
        if (count == 0) {
            throw std::runtime_error(fmt::format(
                "write made no progress for partial output '{}'", partial_path_.string()));
        }
        written += static_cast<std::size_t>(count);
    }
}

void AtomicFile::commit() {
    if (committed_) {
        return;
    }
    if (::fsync(fd_) != 0) {
        throw std::runtime_error(fmt::format(
            "fsync failed for partial output '{}': {}", partial_path_.string(), std::strerror(errno)));
    }
    if (::close(fd_) != 0) {
        fd_ = -1;
        throw std::runtime_error(fmt::format(
            "close failed for partial output '{}': {}", partial_path_.string(), std::strerror(errno)));
    }
    fd_ = -1;

    const int result = overwrite_
        ? ::rename(partial_path_.c_str(), final_path_.c_str())
        : rename_without_replacement(partial_path_, final_path_);
    if (result != 0) {
        throw std::runtime_error(fmt::format(
            "cannot atomically publish '{}' as '{}': {}",
            partial_path_.string(),
            final_path_.string(),
            std::strerror(errno)));
    }
    committed_ = true;
}

const std::filesystem::path& AtomicFile::partial_path() const noexcept {
    return partial_path_;
}

void AtomicFile::cleanup_partial() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (!committed_) {
        ::unlink(partial_path_.c_str());
    }
}

} // namespace recovery_output
