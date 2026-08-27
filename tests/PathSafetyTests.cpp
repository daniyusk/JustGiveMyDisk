#include "PathSafety.hpp"

#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

class TempDirectory {
public:
    TempDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "jgmd-path-safety-XXXXXX").string();
        const char* created = ::mkdtemp(pattern.data());
        if (!created) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = created;
    }

    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Fn>
void require_rejection(Fn&& fn, const std::string& expected, const std::string& label) {
    try {
        fn();
    } catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos,
                label + ": unexpected validation error: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error(label + ": unsafe path was accepted");
}

} // namespace

int main() {
    try {
        TempDirectory temp;
        const auto source_path = temp.path() / "source.img";
        {
            std::ofstream source(source_path, std::ios::binary);
            source << "image";
        }

        path_safety::Source source(source_path);
        const int access_mode = ::fcntl(source.fd(), F_GETFL) & O_ACCMODE;
        require(access_mode == O_RDONLY, "source descriptor is not read-only");

        require_rejection(
            [&] { path_safety::validate_database_path(source, source_path); },
            "alias, symlink, or equivalent path",
            "same database path");

        const auto symlink_path = temp.path() / "source-alias";
        std::filesystem::create_symlink(source_path, symlink_path);
        std::error_code equivalent_error;
        if (std::filesystem::equivalent(source_path, symlink_path, equivalent_error)) {
            require_rejection(
                [&] { path_safety::validate_database_path(source, symlink_path); },
                "alias, symlink, or equivalent path",
                "symlink database path");
        }

        const auto hardlink_path = temp.path() / "source-hardlink";
        std::filesystem::create_hard_link(source_path, hardlink_path);
        require_rejection(
            [&] { path_safety::validate_database_path(source, hardlink_path); },
            "alias, symlink, or equivalent path",
            "hardlink database path");

        const auto destination_file = temp.path() / "destination-file";
        std::ofstream(destination_file) << "data";
        require_rejection(
            [&] { path_safety::validate_destination_path(source, destination_file); },
            "destination is a file",
            "file destination");

        const auto destination_dir = temp.path() / "new" / "destination";
        path_safety::validate_destination_path(source, destination_dir);
        path_safety::validate_database_path(source, temp.path() / "index" / "scan.db");

        std::cout << "path safety tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "path safety tests failed: " << error.what() << '\n';
        return 1;
    }
}
