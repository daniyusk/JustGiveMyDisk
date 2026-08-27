#include "RecoveryOutput.hpp"

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
        auto pattern = (std::filesystem::temp_directory_path() / "jgmd-output-XXXXXX").string();
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
void require_rejection(Fn&& action, const std::string& expected) {
    try {
        action();
    } catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos,
                "unexpected error: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("unsafe output operation was accepted");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

int main() {
    try {
        TempDirectory temp;

        recovery_output::CollisionTracker collisions;
        collisions.add(recovery_output::sanitize_name("reports/2026"), "reports/2026", 10);
        require_rejection(
            [&] { collisions.add(recovery_output::sanitize_name("reports\\2026"), "reports\\2026", 11); },
            "sanitized output collision");

        collisions.add(recovery_output::sanitize_name("alpha.txt"), "alpha.txt", 20);
        require_rejection(
            [&] { collisions.add(recovery_output::sanitize_name("ALPHA.TXT"), "ALPHA.TXT", 21); },
            "sanitized output collision");

        const auto completed = temp.path() / "completed.bin";
        {
            recovery_output::AtomicFile output(completed, false);
            const std::string content = "complete";
            output.write(content.data(), content.size());
            require(std::filesystem::exists(output.partial_path()), "partial file is not visible during recovery");
            require(!std::filesystem::exists(completed), "final file became visible before commit");
            output.commit();
        }
        require(read_file(completed) == "complete", "committed output content differs");
        require(!std::filesystem::exists(completed.string() + ".partial"), "partial remained after commit");

        require_rejection(
            [&] { recovery_output::AtomicFile output(completed, false); },
            "use --overwrite");
        require(read_file(completed) == "complete", "default policy overwrote an existing file");

        {
            recovery_output::AtomicFile output(completed, true);
            const std::string replacement = "replacement";
            output.write(replacement.data(), replacement.size());
            output.commit();
        }
        require(read_file(completed) == "replacement", "explicit overwrite did not replace output");

        const auto interrupted = temp.path() / "interrupted.bin";
        {
            recovery_output::AtomicFile output(interrupted, false);
            const std::string incomplete = "incomplete";
            output.write(incomplete.data(), incomplete.size());
        }
        require(!std::filesystem::exists(interrupted), "interrupted final output exists");
        require(!std::filesystem::exists(interrupted.string() + ".partial"), "interrupted partial was not removed");

        const auto stale_final = temp.path() / "stale.bin";
        const auto stale_partial = std::filesystem::path(stale_final.string() + ".partial");
        std::ofstream(stale_partial) << "stale";
        require_rejection(
            [&] { recovery_output::AtomicFile output(stale_final, false); },
            "stale partial output");
        require(std::filesystem::exists(stale_partial), "stale partial was not clearly preserved");

        {
            recovery_output::AtomicFile output(stale_final, true);
            const std::string recovered = "recovered";
            output.write(recovered.data(), recovered.size());
            output.commit();
        }
        require(read_file(stale_final) == "recovered", "overwrite did not replace stale partial safely");

        const auto symlink_target = temp.path() / "symlink-target.bin";
        std::ofstream(symlink_target) << "target";
        const auto symlink_output = temp.path() / "symlink-out.bin";
        std::error_code symlink_ec;
        std::filesystem::create_symlink(symlink_target, symlink_output, symlink_ec);
        if (!symlink_ec && std::filesystem::is_symlink(symlink_output, symlink_ec)) {
            require_rejection(
                [&] { recovery_output::AtomicFile output(symlink_output, true); },
                "refusing to replace output symlink");
        }

        std::cout << "recovery output tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "recovery output tests failed: " << error.what() << '\n';
        return 1;
    }
}
