#include "Database.hpp"
#include "Scanner.hpp"

#include <CLI/CLI.hpp>
#include <fmt/core.h>

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace {

void print_stored_record(const StoredRecord& record) {
    fmt::print("id={} record_id_guess={} offset={} parent_ref={} name=\"{}\" ns={} dir={} alloc={} real={} flags=0x{:x}\n",
               record.id,
               record.record_id_guess,
               record.offset,
               record.parent_ref,
               record.name,
               record.name_namespace,
               record.is_directory ? 1 : 0,
               record.allocated_size,
               record.real_size,
               record.flags);
}

void print_tree(Database& db, std::uint64_t parent_ref, int depth, int max_depth) {
    if (depth > max_depth) {
        return;
    }

    const auto children = db.children_of(parent_ref);
    for (const auto& child : children) {
        fmt::print("{}{}{} [db_id={}, record={}]\n",
                   std::string(static_cast<std::size_t>(depth) * 2, ' '),
                   child.is_directory ? "[D] " : "[F] ",
                   child.name,
                   child.id,
                   child.record_id_guess);
        if (child.is_directory && child.record_id_guess != parent_ref) {
            print_tree(db, child.record_id_guess, depth + 1, max_depth);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"JustGiveMyDisk - read-only NTFS MFT scanner/indexer"};
    app.require_subcommand(1);

    ScanOptions scan_options;
    auto* scan_cmd = app.add_subcommand("scan", "Scan a source device/image for NTFS FILE records");
    scan_cmd->add_option("source", scan_options.source, "Source block device or image")
        ->required()
        ->check(CLI::ExistingPath);
    scan_cmd->add_option("--out", scan_options.database_path, "SQLite output database")
        ->required();
    scan_cmd->add_option("--target", scan_options.target, "Print matching filenames live while scanning");

    std::string find_db;
    std::string find_name;
    auto* find_cmd = app.add_subcommand("find", "Find records by exact filename");
    find_cmd->add_option("database", find_db, "SQLite scan database")
        ->required()
        ->check(CLI::ExistingFile);
    find_cmd->add_option("name", find_name, "Exact filename to find")
        ->required();

    std::string tree_db;
    std::int64_t tree_id = 0;
    int tree_depth = 4;
    auto* tree_cmd = app.add_subcommand("tree", "Print indexed children below a database row id");
    tree_cmd->add_option("database", tree_db, "SQLite scan database")
        ->required()
        ->check(CLI::ExistingFile);
    tree_cmd->add_option("--id", tree_id, "Database record id to use as the tree root")
        ->required();
    tree_cmd->add_option("--depth", tree_depth, "Maximum recursion depth");

    CLI11_PARSE(app, argc, argv);

    try {
        if (*scan_cmd) {
            Scanner scanner;
            scanner.run(scan_options);
            return 0;
        }

        if (*find_cmd) {
            Database db(find_db);
            const auto rows = db.find_by_name(find_name);
            for (const auto& row : rows) {
                print_stored_record(row);
            }
            fmt::print("{} result(s)\n", rows.size());
            return rows.empty() ? 2 : 0;
        }

        if (*tree_cmd) {
            Database db(tree_db);
            const auto root = db.get_by_id(tree_id);
            if (!root) {
                fmt::print(stderr, "no record with id {}\n", tree_id);
                return 2;
            }
            print_stored_record(*root);
            print_tree(db, root->record_id_guess, 1, tree_depth);
            return 0;
        }
    } catch (const std::exception& e) {
        fmt::print(stderr, "error: {}\n", e.what());
        return 1;
    }

    return 0;
}
