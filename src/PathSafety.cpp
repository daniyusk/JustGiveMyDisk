#include "PathSafety.hpp"

#include <fmt/core.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <utility>

namespace path_safety {
namespace {

[[noreturn]] void fail(const char* role, const std::filesystem::path& path, const std::string& reason) {
    throw std::runtime_error(fmt::format(
        "path validation failed for {} '{}': {}", role, path.string(), reason));
}

std::filesystem::path canonicalize(const std::filesystem::path& path, const char* role) {
    if (path.empty()) {
        fail(role, path, "path is empty");
    }

    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        fail(role, path, fmt::format("cannot make path absolute: {}", ec.message()));
    }
    const auto canonical = std::filesystem::weakly_canonical(absolute, ec);
    if (ec) {
        fail(role, path, fmt::format("cannot safely canonicalize path: {}", ec.message()));
    }
    return canonical;
}

bool same_opened_object(const struct stat& lhs, const struct stat& rhs) {
    if ((S_ISBLK(lhs.st_mode) || S_ISCHR(lhs.st_mode)) &&
        (S_ISBLK(rhs.st_mode) || S_ISCHR(rhs.st_mode))) {
        return lhs.st_rdev == rhs.st_rdev;
    }
    return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino;
}

struct stat stat_or_fail(const std::filesystem::path& path, const char* role) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        fail(role, path, fmt::format("stat failed: {}", std::strerror(errno)));
    }
    return info;
}

struct ExistingAncestor {
    std::filesystem::path path;
    struct stat info {};
};

ExistingAncestor existing_ancestor(const std::filesystem::path& canonical_path, const char* role) {
    auto current = canonical_path;
    while (true) {
        struct stat info {};
        if (::stat(current.c_str(), &info) == 0) {
            return {current, info};
        }
        if (errno != ENOENT && errno != ENOTDIR) {
            fail(role, canonical_path, fmt::format(
                "cannot inspect '{}' while resolving storage: {}", current.string(), std::strerror(errno)));
        }
        const auto parent = current.parent_path();
        if (parent == current || parent.empty()) {
            fail(role, canonical_path, "no existing ancestor could be inspected");
        }
        current = parent;
    }
}

std::optional<dev_t> read_sysfs_device_number(const std::filesystem::path& directory) {
    std::ifstream input(directory / "dev");
    unsigned int device_major = 0;
    unsigned int device_minor = 0;
    char separator = 0;
    if (!(input >> device_major >> separator >> device_minor) || separator != ':') {
        return std::nullopt;
    }
    return makedev(device_major, device_minor);
}

void add_device_relations(dev_t device, std::set<dev_t>& devices) {
    if (!devices.insert(device).second) {
        return;
    }

    std::error_code ec;
    auto sys_path = std::filesystem::canonical(
        std::filesystem::path("/sys/dev/block") /
            fmt::format("{}:{}", major(device), minor(device)),
        ec);
    if (ec) {
        return;
    }

    for (auto current = sys_path; !current.empty(); current = current.parent_path()) {
        if (const auto parent_device = read_sysfs_device_number(current)) {
            devices.insert(*parent_device);
        }
        if (current == current.root_path()) {
            break;
        }
    }

    const auto slaves = sys_path / "slaves";
    if (!std::filesystem::is_directory(slaves, ec)) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(slaves, ec)) {
        if (ec) {
            break;
        }
        const auto slave_path = std::filesystem::canonical(entry.path(), ec);
        if (ec) {
            continue;
        }
        if (const auto slave_device = read_sysfs_device_number(slave_path)) {
            add_device_relations(*slave_device, devices);
        }
    }
}

bool devices_are_related(dev_t source, dev_t storage) {
    if (source == storage) {
        return true;
    }
    std::set<dev_t> source_relations;
    std::set<dev_t> storage_relations;
    add_device_relations(source, source_relations);
    add_device_relations(storage, storage_relations);
    for (const auto device : source_relations) {
        if (storage_relations.contains(device)) {
            return true;
        }
    }
    return false;
}

void reject_source_storage(const Source& source,
                           const std::filesystem::path& path,
                           const char* role,
                           const ExistingAncestor& storage) {
    if (!source.is_block_device()) {
        return;
    }
    const auto source_device = static_cast<dev_t>(source.block_device());
    if (devices_are_related(source_device, storage.info.st_dev)) {
        fail(role, path, fmt::format(
            "'{}' is stored on the source device (source {}:{}, filesystem {}:{})",
            storage.path.string(),
            major(source_device),
            minor(source_device),
            major(storage.info.st_dev),
            minor(storage.info.st_dev)));
    }
}

void reject_source_alias(const Source& source,
                         const std::filesystem::path& original,
                         const std::filesystem::path& canonical,
                         const char* role) {
    struct stat candidate {};
    if (::stat(canonical.c_str(), &candidate) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return;
        }
        fail(role, original, fmt::format("stat failed: {}", std::strerror(errno)));
    }

    struct stat opened_source {};
    if (::fstat(source.fd(), &opened_source) != 0) {
        fail(role, original, fmt::format("fstat on the open source failed: {}", std::strerror(errno)));
    }
    if (same_opened_object(opened_source, candidate)) {
        fail(role, original, fmt::format(
            "resolves to the open recovery source '{}' (alias, symlink, or equivalent path)",
            source.canonical_path().string()));
    }
}

} // namespace

Source::Source(const std::filesystem::path& path)
    : canonical_path_(canonicalize(path, "source")) {
    const auto before = stat_or_fail(canonical_path_, "source");
    if (!S_ISREG(before.st_mode) && !S_ISBLK(before.st_mode)) {
        fail("source", path, "source must be a regular image file or a block device");
    }

    int open_flags = O_RDONLY;
#ifdef O_CLOEXEC
    open_flags |= O_CLOEXEC;
#endif
    fd_ = ::open(canonical_path_.c_str(), open_flags);
    if (fd_ < 0) {
        fail("source", path, fmt::format("cannot open read-only: {}", std::strerror(errno)));
    }

    struct stat after {};
    if (::fstat(fd_, &after) != 0) {
        const auto error = std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
        fail("source", path, fmt::format("fstat failed after opening: {}", error));
    }
    if (!same_opened_object(before, after)) {
        ::close(fd_);
        fd_ = -1;
        fail("source", path, "path changed between stat and read-only open");
    }
#ifndef O_CLOEXEC
    if (::fcntl(fd_, F_SETFD, FD_CLOEXEC) != 0) {
        const auto error = std::strerror(errno);
        ::close(fd_);
        fd_ = -1;
        fail("source", path, fmt::format("cannot set close-on-exec: {}", error));
    }
#endif
    const int flags = ::fcntl(fd_, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY) {
        ::close(fd_);
        fd_ = -1;
        fail("source", path, "open descriptor is not read-only");
    }

    is_block_device_ = S_ISBLK(after.st_mode);
    block_device_ = static_cast<std::uint64_t>(after.st_rdev);
}

Source::~Source() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

Source::Source(Source&& other) noexcept
    : fd_(other.fd_),
      canonical_path_(std::move(other.canonical_path_)),
      block_device_(other.block_device_),
      is_block_device_(other.is_block_device_) {
    other.fd_ = -1;
}

Source& Source::operator=(Source&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = other.fd_;
        canonical_path_ = std::move(other.canonical_path_);
        block_device_ = other.block_device_;
        is_block_device_ = other.is_block_device_;
        other.fd_ = -1;
    }
    return *this;
}

int Source::fd() const noexcept {
    return fd_;
}

const std::filesystem::path& Source::canonical_path() const noexcept {
    return canonical_path_;
}

bool Source::is_block_device() const noexcept {
    return is_block_device_;
}

std::uint64_t Source::block_device() const noexcept {
    return block_device_;
}

void validate_database_path(const Source& source, const std::filesystem::path& database_path) {
    const auto canonical = canonicalize(database_path, "database");
    reject_source_alias(source, database_path, canonical, "database");
    const auto storage = existing_ancestor(canonical, "database");
    if (!S_ISREG(storage.info.st_mode) && storage.path == canonical) {
        fail("database", database_path, "existing database path is not a regular file");
    }
    if (storage.path != canonical && !S_ISDIR(storage.info.st_mode)) {
        fail("database", database_path, fmt::format(
            "existing ancestor '{}' is not a directory", storage.path.string()));
    }
    reject_source_storage(source, database_path, "database", storage);
}

void validate_destination_path(const Source& source, const std::filesystem::path& destination_path) {
    const auto canonical = canonicalize(destination_path, "destination");
    reject_source_alias(source, destination_path, canonical, "destination");
    const auto storage = existing_ancestor(canonical, "destination");
    if (storage.path == canonical && !S_ISDIR(storage.info.st_mode)) {
        if (S_ISREG(storage.info.st_mode)) {
            fail("destination", destination_path, "destination is a file; a directory is required");
        }
        if (S_ISBLK(storage.info.st_mode)) {
            fail("destination", destination_path, "destination is a block device; a directory is required");
        }
        fail("destination", destination_path, "destination is not a directory");
    }
    if (storage.path != canonical && !S_ISDIR(storage.info.st_mode)) {
        fail("destination", destination_path, fmt::format(
            "existing ancestor '{}' is not a directory", storage.path.string()));
    }
    reject_source_storage(source, destination_path, "destination", storage);
}

} // namespace path_safety
