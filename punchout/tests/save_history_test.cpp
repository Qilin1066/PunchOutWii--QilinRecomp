#include "save_history.hpp"
#include <chrono>
#include <fstream>
#include <iterator>
#include <cstdio>
namespace fs = std::filesystem;
int main() {
    const fs::path folder = fs::temp_directory_path() / ("punchout-history-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path nand = folder / "nand", save = nand / "title/game/save.dat";
    fs::create_directories(save.parent_path());
    int failures = 0;
    const auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok" : "FAIL", what); if (!ok) ++failures; };
    const auto write = [&](const std::string& text) { std::ofstream out(save, std::ios::binary); out << text; };
    const auto read = [&](const fs::path& path) { std::ifstream in(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(in), {}); };
    std::string error;
    for (int i = 0; i < 8; ++i) {
        write("version " + std::to_string(i));
        check(punchout::backup_save(nand, save, error), "backup succeeds");
    }
    auto backups = punchout::save_backups(folder);
    check(backups.size() == 5, "history bounded to five per save");
    const auto selected = backups.back().file; // oldest retained: exercises pruning during restore
    const std::string selected_data = read(selected);
    write("current save");
    check(punchout::request_save_restore(folder, selected, error), "restore request queued");
    check(read(save) == "current save", "queued restore leaves running game's save untouched");
    check(punchout::apply_save_restore(folder, error) && read(save) == selected_data, "restore applied before next boot");
    bool current_preserved = false;
    for (const auto& item : punchout::save_backups(folder)) current_preserved |= read(item.file) == "current save";
    check(current_preserved, "pre-restore save can be recovered");
    check(!fs::exists(folder / "restore-save.txt"), "completed restore request removed");
    check(!punchout::request_save_restore(folder, folder / "../outside.bak", error), "outside path rejected");
    { std::ofstream invalid(folder / "restore-save.txt"); invalid << "../../outside.bak\n"; }
    check(!punchout::apply_save_restore(folder, error) && read(save) == selected_data, "bad request cannot modify current save");
    fs::remove(folder / "restore-save.txt");
    // A stale staging file from an interrupted operation is never offered as a backup.
    { std::ofstream partial(backups.front().file.string() + ".tmp"); partial << "partial"; }
    check(punchout::save_backups(folder).size() == 5, "incomplete backups ignored");
    fs::create_directories(folder / "save-backups/title/game/blocked.dat.history");
    const fs::path blocked = nand / "title/game/blocked.dat";
    fs::create_directory(blocked);
    check(!punchout::backup_save(nand, blocked, error), "non-file destination reports failure");
    fs::remove_all(folder);
    return failures ? 1 : 0;
}
