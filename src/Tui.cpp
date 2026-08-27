#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE
#endif
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "Tui.hpp"

#include "Database.hpp"
#include "Recover.hpp"
#include "Scanner.hpp"

#include <fmt/core.h>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <stdio.h>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(__CYGWIN__) || defined(__MSYS__)
extern "C" {
FILE* popen(const char*, const char*);
int pclose(FILE*);
}
#endif

namespace {

using namespace ftxui;

struct BlockDevice {
    std::string path;
    std::string type;
    std::string size;
    std::string fstype;
    std::string label;
    std::string model;
    std::string mountpoint;
};

enum class Page {
    Devices,
    Warning,
    Scanning,
    Search,
    Results,
    Actions,
    TextView,
    RecoverDest,
    ConfirmRecover,
};

std::string run_command(const std::string& command) {
    std::array<char, 4096> buffer {};
    std::string output;
#if defined(_WIN32)
    FILE* pipe = ::_popen(command.c_str(), "r");
#else
    FILE* pipe = ::popen(command.c_str(), "r");
#endif
    if (!pipe) {
        return output;
    }
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
#if defined(_WIN32)
    ::_pclose(pipe);
#else
    ::pclose(pipe);
#endif
    return output;
}

std::optional<std::string> json_string_field(const std::string& object, const std::string& key) {
    const std::string marker = "\"" + key + "\":";
    const auto pos = object.find(marker);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    auto value = pos + marker.size();
    while (value < object.size() && object[value] == ' ') {
        ++value;
    }
    if (value + 4 <= object.size() && object.substr(value, 4) == "null") {
        return "";
    }
    if (value >= object.size() || object[value] != '"') {
        return std::nullopt;
    }
    ++value;
    std::string result;
    bool escaped = false;
    for (; value < object.size(); ++value) {
        const char ch = object[value];
        if (escaped) {
            result.push_back(ch);
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            return result;
        } else {
            result.push_back(ch);
        }
    }
    return std::nullopt;
}

std::optional<std::string> json_number_field(const std::string& object, const std::string& key) {
    const std::string marker = "\"" + key + "\":";
    const auto pos = object.find(marker);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    auto value = pos + marker.size();
    while (value < object.size() && object[value] == ' ') {
        ++value;
    }
    std::string result;
    while (value < object.size() && object[value] >= '0' && object[value] <= '9') {
        result.push_back(object[value++]);
    }
    if (result.empty()) {
        return std::nullopt;
    }
    return result;
}

std::string human_size(const std::string& bytes_text) {
    if (bytes_text.empty()) {
        return "";
    }
    double bytes = 0.0;
    try {
        bytes = std::stod(bytes_text);
    } catch (...) {
        return bytes_text;
    }

    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit = 0;
    while (bytes >= 1024.0 && unit < 4) {
        bytes /= 1024.0;
        ++unit;
    }
    return fmt::format("{:.1f} {}", bytes, units[unit]);
}

bool allowed_block_path(const std::string& path) {
    return path.rfind("/dev/nvme", 0) == 0 || path.rfind("/dev/sd", 0) == 0;
}

std::vector<std::string> json_objects(const std::string& json) {
    std::vector<std::string> objects;
    std::vector<std::size_t> starts;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = 0; i < json.size(); ++i) {
        const char ch = json[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '{') {
            starts.push_back(i);
        } else if (ch == '}') {
            if (!starts.empty()) {
                const auto start = starts.back();
                starts.pop_back();
                objects.push_back(json.substr(start, i - start + 1));
            }
        }
    }
    return objects;
}

std::vector<BlockDevice> detect_devices() {
    std::vector<BlockDevice> devices;
    const auto json = run_command("lsblk -J -p -b -o PATH,TYPE,SIZE,FSTYPE,LABEL,MODEL,MOUNTPOINT 2>/dev/null");
    for (const auto& object : json_objects(json)) {
        const auto path = json_string_field(object, "path");
        const auto type = json_string_field(object, "type").value_or("");
        if (!path || path->empty() || !allowed_block_path(*path)) {
            continue;
        }

        BlockDevice device;
        device.path = *path;
        device.type = type;
        device.size = human_size(json_number_field(object, "size").value_or(""));
        device.fstype = json_string_field(object, "fstype").value_or("");
        device.label = json_string_field(object, "label").value_or("");
        device.model = json_string_field(object, "model").value_or("");
        device.mountpoint = json_string_field(object, "mountpoint").value_or("");
        devices.push_back(std::move(device));
    }

    std::sort(devices.begin(), devices.end(), [](const BlockDevice& lhs, const BlockDevice& rhs) {
        return lhs.path < rhs.path;
    });
    devices.erase(std::unique(devices.begin(), devices.end(), [](const BlockDevice& lhs, const BlockDevice& rhs) {
        return lhs.path == rhs.path;
    }), devices.end());

    if (devices.empty()) {
        const std::filesystem::path sys_block = "/sys/block";
        if (std::filesystem::exists(sys_block)) {
            for (const auto& entry : std::filesystem::directory_iterator(sys_block)) {
                const auto name = entry.path().filename().string();
                const std::string root_path = "/dev/" + name;
                if (!allowed_block_path(root_path)) {
                    continue;
                }
                devices.push_back(BlockDevice{root_path, "disk", "", "", "", "", ""});
                for (const auto& child : std::filesystem::directory_iterator(entry.path())) {
                    const auto child_name = child.path().filename().string();
                    const std::string child_path = "/dev/" + child_name;
                    if (child_name.rfind(name, 0) == 0 && allowed_block_path(child_path)) {
                        devices.push_back(BlockDevice{child_path, "part", "", "", "", "", ""});
                    }
                }
            }
        }
    }

    return devices;
}

std::string device_line(const BlockDevice& device) {
    return fmt::format("{:<18} {:<5} {:<10} fs={:<8} label={:<16} model={:<18} {}",
                       device.path,
                       device.type,
                       device.size,
                       device.fstype.empty() ? "-" : device.fstype,
                       device.label.empty() ? "-" : device.label,
                       device.model.empty() ? "-" : device.model,
                       device.mountpoint.empty() ? "" : device.mountpoint);
}

std::string record_line(const StoredRecord& record) {
    return fmt::format("{} record={} parent={} name=\"{}\" db_id={}",
                       record.is_directory ? "[D]" : "[F]",
                       record.record_id_guess,
                       record.parent_ref,
                       record.name,
                       record.id);
}

void append_log(std::vector<std::string>& logs, const std::string& message) {
    logs.push_back(message);
    if (logs.size() > 200) {
        logs.erase(logs.begin(), logs.begin() + static_cast<std::ptrdiff_t>(logs.size() - 200));
    }
}

Elements log_elements(const std::vector<std::string>& logs, int max_lines = 12) {
    Elements lines;
    const auto start = logs.size() > static_cast<std::size_t>(max_lines)
        ? logs.size() - static_cast<std::size_t>(max_lines)
        : 0;
    for (std::size_t i = start; i < logs.size(); ++i) {
        lines.push_back(text(logs[i]));
    }
    if (lines.empty()) {
        lines.push_back(text(""));
    }
    return lines;
}

std::string tree_preview(Database& db,
                         std::uint64_t parent,
                         int depth,
                         std::unordered_set<std::uint64_t>& visited) {
    if (depth > 6 || !visited.insert(parent).second) {
        return "";
    }

    std::ostringstream out;
    for (const auto& child : db.children_of(parent)) {
        out << std::string(static_cast<std::size_t>(depth) * 2, ' ')
            << (child.is_directory ? "[D] " : "[F] ")
            << child.name << " record=" << child.record_id_guess << "\n";
        if (child.is_directory) {
            out << tree_preview(db, child.record_id_guess, depth + 1, visited);
        }
    }
    return out.str();
}

std::string parent_chain(Database& db, StoredRecord record) {
    std::ostringstream out;
    std::unordered_set<std::uint64_t> visited;
    while (true) {
        out << record_line(record) << "\n";
        if (record.parent_ref == 0 || !visited.insert(record.record_id_guess).second) {
            break;
        }
        const auto parent = db.get_by_record_id(record.parent_ref);
        if (!parent) {
            out << "parent record " << record.parent_ref << " not found in index\n";
            break;
        }
        record = *parent;
    }
    return out.str();
}

} // namespace

int run_tui() {
    auto screen = ScreenInteractive::Fullscreen();

    Page page = Page::Devices;
    std::vector<BlockDevice> devices = detect_devices();
    std::vector<std::string> device_entries;
    for (const auto& device : devices) {
        device_entries.push_back(device_line(device));
    }
    if (device_entries.empty()) {
        device_entries.push_back("No /dev/nvme* or /dev/sd* devices found");
    }

    int device_index = 0;
    int result_index = 0;
    int action_index = 0;
    int search_mode = 0;
    bool scan_running = false;
    bool scan_done = false;
    bool real_recover_confirmed = false;
    std::optional<BlockDevice> selected_device;
    std::optional<StoredRecord> selected_record;
    std::string database_path = "scan.db";
    std::string search_text;
    std::string destination_path;
    std::string text_title;
    std::string text_body;
    std::vector<StoredRecord> results;
    std::vector<std::string> result_entries;
    std::vector<std::string> logs;
    ScanStats scan_stats;
    std::optional<std::uint64_t> scan_total;
    std::mutex state_mutex;
    std::thread worker;

    auto join_worker = [&]() {
        if (worker.joinable()) {
            worker.join();
        }
    };

    auto start_scan = [&]() {
        if (!selected_device || scan_running) {
            return;
        }
        scan_running = true;
        scan_done = false;
        logs.clear();
        ScanOptions options;
        options.source = selected_device->path;
        options.database_path = database_path;

        join_worker();
        worker = std::thread([&, options]() {
            try {
                Scanner scanner;
                scanner.run(options, [&](const ScanProgress& progress) {
                    {
                        std::lock_guard lock(state_mutex);
                        scan_stats = progress.stats;
                        scan_total = progress.total_size;
                        append_log(logs, progress.message);
                    }
                    screen.PostEvent(Event::Custom);
                });
            } catch (const std::exception& e) {
                std::lock_guard lock(state_mutex);
                append_log(logs, fmt::format("error: {}", e.what()));
            }
            {
                std::lock_guard lock(state_mutex);
                scan_running = false;
                scan_done = true;
            }
            screen.PostEvent(Event::Custom);
        });
    };

    auto do_search = [&]() {
        result_entries.clear();
        results.clear();
        selected_record.reset();
        try {
            Database db(database_path);
            for (const auto& row : db.find_by_name(search_text)) {
                if ((search_mode == 0 && row.is_directory) || (search_mode == 1 && !row.is_directory)) {
                    results.push_back(row);
                    result_entries.push_back(record_line(row));
                }
            }
            if (result_entries.empty()) {
                result_entries.push_back("No matching records");
            }
            result_index = 0;
            page = Page::Results;
        } catch (const std::exception& e) {
            text_title = "Search error";
            text_body = e.what();
            page = Page::TextView;
        }
    };

    auto show_parent_chain = [&]() {
        if (!selected_record) {
            return;
        }
        try {
            Database db(database_path);
            text_title = "Parent chain";
            text_body = parent_chain(db, *selected_record);
            page = Page::TextView;
        } catch (const std::exception& e) {
            text_title = "Parent chain error";
            text_body = e.what();
            page = Page::TextView;
        }
    };

    auto show_tree = [&]() {
        if (!selected_record) {
            return;
        }
        try {
            Database db(database_path);
            std::unordered_set<std::uint64_t> visited;
            text_title = selected_record->is_directory ? "Folder tree" : "Containing folder tree";
            const auto root = selected_record->is_directory ? selected_record->record_id_guess : selected_record->parent_ref;
            text_body = tree_preview(db, root, 0, visited);
            if (text_body.empty()) {
                text_body = "No children found in index";
            }
            page = Page::TextView;
        } catch (const std::exception& e) {
            text_title = "Tree error";
            text_body = e.what();
            page = Page::TextView;
        }
    };

    auto run_recover = [&](bool dry_run) {
        if (!selected_device || !selected_record || !selected_record->is_directory) {
            return;
        }
        logs.clear();
        RecoverOptions options;
        options.source = selected_device->path;
        options.database_path = database_path;
        options.root_record_id = selected_record->record_id_guess;
        options.destination = destination_path;
        options.dry_run = dry_run;
        try {
            Recover recover;
            const auto stats = recover.run(options, [&](const std::string& message) {
                append_log(logs, message);
            });
            text_title = dry_run ? "Dry-run recover" : "Recover complete";
            text_body = fmt::format("{}\npreview_items={} recovered={} skipped={}",
                                    logs.empty() ? "" : logs.back(),
                                    stats.preview_items,
                                    stats.recovered,
                                    stats.skipped);
        } catch (const std::exception& e) {
            text_title = dry_run ? "Dry-run recover error" : "Recover error";
            text_body = e.what();
        }
        page = Page::TextView;
    };

    auto device_menu = Menu(&device_entries, &device_index);
    auto db_input = Input(&database_path, "scan.db");
    auto search_input = Input(&search_text, "name");
    std::vector<std::string> modes = {"folder name", "file name"};
    auto mode_radio = Radiobox(&modes, &search_mode);
    auto result_menu = Menu(&result_entries, &result_index);
    std::vector<std::string> actions;
    auto action_menu = Menu(&actions, &action_index);
    auto dest_input = Input(&destination_path, "/path/to/recovery");

    auto component = Container::Vertical({
        device_menu,
        db_input,
        search_input,
        mode_radio,
        result_menu,
        action_menu,
        dest_input,
    });

    auto renderer = Renderer(component, [&] {
        std::lock_guard lock(state_mutex);
        const auto shortcuts = text("q back/quit  Enter select  / search  r recover folder  d dry-run folder") | dim;

        if (page == Page::Devices) {
            return vbox({
                text("JustGiveMyDisk TUI") | bold,
                text("Select a Linux block device or partition"),
                separator(),
                device_menu->Render() | frame | flex,
                separator(),
                shortcuts,
            }) | border;
        }

        if (page == Page::Warning) {
            return vbox({
                text("Read-only scan warning") | bold,
                paragraph("The source device will be opened read-only. JustGiveMyDisk will not write to the source partition. The SQLite index path below is the only scan output."),
                separator(),
                text(selected_device ? device_line(*selected_device) : ""),
                hbox(text("scan database: "), db_input->Render() | flex),
                separator(),
                text("Enter starts scanning. q returns to device selection."),
                shortcuts,
            }) | border;
        }

        if (page == Page::Scanning) {
            double progress = 0.0;
            if (scan_total && *scan_total > 0) {
                progress = std::min(1.0, static_cast<double>(scan_stats.bytes_scanned) / static_cast<double>(*scan_total));
            }
            return vbox({
                text(scan_running ? "Scanning" : "Scan complete") | bold,
                text(selected_device ? selected_device->path : ""),
                gauge(static_cast<float>(progress)),
                text(fmt::format("scanned={} valid_records={} names={}",
                                 scan_stats.bytes_scanned,
                                 scan_stats.valid_records,
                                 scan_stats.inserted_names)),
                separator(),
                vbox(log_elements(logs)) | frame | flex,
                separator(),
                text(scan_done ? "Enter continues to search." : "Scanning, please wait."),
                shortcuts,
            }) | border;
        }

        if (page == Page::Search) {
            return vbox({
                text("Search indexed records") | bold,
                text(fmt::format("database: {}", database_path)),
                separator(),
                mode_radio->Render(),
                hbox(text("search: "), search_input->Render() | flex),
                separator(),
                text("Enter runs search. / focuses this screen."),
                shortcuts,
            }) | border;
        }

        if (page == Page::Results) {
            return vbox({
                text("Search results") | bold,
                result_menu->Render() | frame | flex,
                separator(),
                shortcuts,
            }) | border;
        }

        if (page == Page::Actions) {
            return vbox({
                text("Record actions") | bold,
                text(selected_record ? record_line(*selected_record) : ""),
                separator(),
                action_menu->Render() | frame | flex,
                separator(),
                shortcuts,
            }) | border;
        }

        if (page == Page::RecoverDest) {
            return vbox({
                text("Recovery destination") | bold,
                text(selected_record ? record_line(*selected_record) : ""),
                hbox(text("dest: "), dest_input->Render() | flex),
                separator(),
                text("Enter runs dry-run first. q returns."),
                shortcuts,
            }) | border;
        }

        if (page == Page::ConfirmRecover) {
            return vbox({
                text("Confirm real recovery") | bold,
                paragraph("Dry-run/preview completed. Press Enter to perform real recovery into the destination path, or q to cancel."),
                text(fmt::format("dest: {}", destination_path)),
                separator(),
                vbox(log_elements(logs)) | frame | flex,
                shortcuts,
            }) | border;
        }

        return vbox({
            text(text_title) | bold,
            separator(),
            paragraph(text_body) | flex,
            separator(),
            text("q returns."),
            shortcuts,
        }) | border;
    });

    renderer = CatchEvent(renderer, [&](Event event) {
        if (event == Event::Character('q')) {
            if (page == Page::Devices) {
                screen.ExitLoopClosure()();
            } else if (page == Page::Results) {
                page = Page::Search;
            } else if (page == Page::Warning) {
                page = Page::Devices;
            } else if (page == Page::Scanning && scan_done) {
                page = Page::Search;
            } else {
                page = Page::Results;
            }
            return true;
        }

        if (event == Event::Character('/')) {
            page = Page::Search;
            return true;
        }

        if (event == Event::Character('d') && selected_record && selected_record->is_directory) {
            if (destination_path.empty()) {
                destination_path = (std::filesystem::current_path() / "recovered").string();
            }
            run_recover(true);
            return true;
        }

        if (event == Event::Character('r') && selected_record && selected_record->is_directory) {
            if (destination_path.empty()) {
                destination_path = (std::filesystem::current_path() / "recovered").string();
            }
            page = Page::RecoverDest;
            return true;
        }

        if (event != Event::Return) {
            return false;
        }

        if (page == Page::Devices) {
            if (!devices.empty() && device_index >= 0 && static_cast<std::size_t>(device_index) < devices.size()) {
                selected_device = devices[static_cast<std::size_t>(device_index)];
                page = Page::Warning;
            }
            return true;
        }
        if (page == Page::Warning) {
            page = Page::Scanning;
            start_scan();
            return true;
        }
        if (page == Page::Scanning && scan_done) {
            join_worker();
            page = Page::Search;
            return true;
        }
        if (page == Page::Search) {
            do_search();
            return true;
        }
        if (page == Page::Results) {
            if (!results.empty() && result_index >= 0 && static_cast<std::size_t>(result_index) < results.size()) {
                selected_record = results[static_cast<std::size_t>(result_index)];
                actions.clear();
                actions.push_back("show parent chain");
                actions.push_back(selected_record->is_directory ? "preview tree" : "open containing folder tree");
                if (selected_record->is_directory) {
                    actions.push_back("dry-run recover");
                    actions.push_back("recover");
                }
                action_index = 0;
                page = Page::Actions;
            }
            return true;
        }
        if (page == Page::Actions) {
            if (action_index == 0) {
                show_parent_chain();
            } else if (action_index == 1) {
                show_tree();
            } else if (selected_record && selected_record->is_directory && action_index == 2) {
                if (destination_path.empty()) {
                    destination_path = (std::filesystem::current_path() / "recovered").string();
                }
                run_recover(true);
            } else if (selected_record && selected_record->is_directory && action_index == 3) {
                if (destination_path.empty()) {
                    destination_path = (std::filesystem::current_path() / "recovered").string();
                }
                page = Page::RecoverDest;
            }
            return true;
        }
        if (page == Page::RecoverDest) {
            real_recover_confirmed = false;
            run_recover(true);
            page = Page::ConfirmRecover;
            return true;
        }
        if (page == Page::ConfirmRecover) {
            if (!real_recover_confirmed) {
                real_recover_confirmed = true;
                run_recover(false);
            }
            return true;
        }
        if (page == Page::TextView) {
            page = Page::Results;
            return true;
        }
        return false;
    });

    screen.Loop(renderer);
    join_worker();
    return 0;
}
