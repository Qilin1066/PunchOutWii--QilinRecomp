#include "save_history.hpp"
#include "qilin/rt/file.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <ctime>
#include <thread>

namespace punchout {
namespace fs = std::filesystem;
namespace {
constexpr size_t kKeep = 5;
constexpr const char* kSuffix = ".history";
bool fail(std::string& error, const std::string& text, const std::error_code& ec = {}) {
    error = text + (ec ? ": " + ec.message() : ""); return false;
}
bool relative_safe(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name()) return false;
    for (const auto& part : path)
        if (part == "." || part == ".." || part.string().find_first_of("\\:") != std::string::npos) return false;
    return true;
}
// Reject symlinks in either the history or the destination, not just '..'.
bool plain_path(const fs::path& root, const fs::path& relative) {
    if (!relative_safe(relative)) return false;
    std::error_code ec;
    fs::path at = root;
    if (fs::is_symlink(fs::symlink_status(root, ec))) return false;
    for (const auto& part : relative) {
        at /= part;
        ec.clear();
        const auto status = fs::symlink_status(at, ec);
        if (fs::is_symlink(status)) return false;
        if (ec && ec != std::errc::no_such_file_or_directory) return false;
    }
    return true;
}
bool copy_flushed(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    fs::copy_file(from, to, fs::copy_options::none, ec);
    if (ec) { const auto reason = ec; fs::remove(to, ec); return fail(error, "cannot copy save", reason); }
#if defined(_WIN32)
    FILE* f = _wfopen(to.c_str(), L"r+b");
#else
    FILE* f = std::fopen(to.c_str(), "r+b");
#endif
    if (!f) { fs::remove(to, ec); return fail(error, "cannot flush save copy"); }
    bool ok = qilin::rt::sync_file(f);
    ok = std::fclose(f) == 0 && ok;
    if (!ok) { fs::remove(to, ec); return fail(error, "save copy flush/close failed"); }
    return true;
}
bool backup_paths(const fs::path& folder, const fs::path& backup, fs::path& relative, fs::path& target, std::string& error) {
    const fs::path history = folder / "save-backups";
    relative = backup.lexically_relative(history);
    if (!plain_path(history, relative) || backup.extension() != ".bak") return fail(error, "invalid backup path");
    std::string parent = relative.parent_path().filename().string();
    if (!parent.ends_with(kSuffix)) return fail(error, "invalid backup directory");
    parent.resize(parent.size() - std::char_traits<char>::length(kSuffix));
    const fs::path destination = relative.parent_path().parent_path() / parent;
    if (!plain_path(folder / "nand", destination)) return fail(error, "invalid save destination");
    target = folder / "nand" / destination;
    std::error_code ec;
    if (!fs::is_regular_file(backup, ec)) return fail(error, "backup is missing", ec);
    return true;
}
}  // namespace

bool backup_save(const fs::path& nand, const fs::path& file, std::string& error) {
    const fs::path relative = file.lexically_relative(nand);
    if (!plain_path(nand, relative)) return fail(error, "invalid save path");
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return fail(error, "save is not a regular file", ec);
    const fs::path history_root = nand.parent_path() / "save-backups";
    fs::path history_relative = relative;
    history_relative += kSuffix;
    if (!plain_path(history_root, history_relative)) return fail(error, "invalid history path");
    const fs::path dir = history_root / history_relative;
    fs::create_directories(dir, ec);
    if (ec) return fail(error, "cannot create save history", ec);
    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    fs::path destination;
    for (unsigned sequence = 0;; ++sequence) {
        std::ostringstream name;
        name << std::setw(20) << std::setfill('0') << ticks << '-' << std::setw(6) << sequence << ".bak";
        destination = dir / name.str();
        if (!fs::exists(destination, ec)) break;
    }
    fs::path temporary = destination; temporary += ".tmp";
    if (!copy_flushed(file, temporary, error)) return false;
    fs::rename(temporary, destination, ec);
    if (ec) { const auto reason = ec; fs::remove(temporary, ec); return fail(error, "cannot commit save backup", reason); }
    std::vector<fs::path> backups;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".bak") backups.push_back(it->path());
    std::sort(backups.begin(), backups.end());
    for (size_t i = 0; i + kKeep < backups.size(); ++i) fs::remove(backups[i], ec);
    error.clear(); return true;
}

std::vector<SaveBackup> save_backups(const fs::path& folder) {
    std::vector<SaveBackup> out;
    const fs::path root = folder / "save-backups";
    std::error_code ec;
    if (!fs::exists(root, ec)) return out;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".bak") continue;
        fs::path relative, target;
        std::string error;
        if (backup_paths(folder, it->path(), relative, target, error))
        {
            const std::string stamp = it->path().stem().string().substr(0, 20);
            const auto seconds = static_cast<std::time_t>(std::strtoll(stamp.c_str(), nullptr, 10) / 1000000);
            std::tm calendar{};
#if defined(_WIN32)
            gmtime_s(&calendar, &seconds);
#else
            gmtime_r(&seconds, &calendar);
#endif
            char label[64]{};
            std::strftime(label, sizeof label, "%Y-%m-%d %H:%M:%S UTC", &calendar);
            const std::string relative_save = target.lexically_relative(folder / "nand").generic_string();
            const std::string kind = relative_save.starts_with("title/00010000/52375050/data/") ? "Game save: " : "Wii data: ";
            out.push_back({it->path(), kind + target.filename().string() + " / " + label});
        }
    }
    std::sort(out.begin(), out.end(), [](const SaveBackup& a, const SaveBackup& b) { return a.file.filename() > b.file.filename(); });
    return out;
}

bool request_save_restore(const fs::path& folder, const fs::path& backup, std::string& error) {
    fs::path relative, target;
    if (!backup_paths(folder, backup, relative, target, error)) return false;
    const fs::path temporary = folder / "restore-save.txt.tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out << relative.generic_string() << '\n';
    out.close();
    if (!out) return fail(error, "cannot request save restore");
    std::error_code ec;
    fs::rename(temporary, folder / "restore-save.txt", ec);
    if (ec) return fail(error, "cannot commit restore request", ec);
    error.clear(); return true;
}

bool apply_save_restore(const fs::path& folder, std::string& error) {
    const fs::path request = folder / "restore-save.txt";
    std::error_code ec;
    if (!fs::exists(request, ec)) return ec ? fail(error, "cannot inspect restore request", ec) : true;
    std::string name, extra;
    {
        // Closed before the request is removed at the end: Windows won't
        // delete a file that is still open.
        std::ifstream in(request, std::ios::binary);
        if (!std::getline(in, name) || std::getline(in, extra)) return fail(error, "invalid restore request");
    }
    fs::path relative, target;
    const fs::path backup = folder / "save-backups" / fs::path(name);
    if (!backup_paths(folder, backup, relative, target, error)) return false;
    // Stage the selected backup before snapshotting the present save: retention
    // may remove the selected old backup when the present copy is added.
    fs::create_directories(target.parent_path(), ec);
    if (ec) return fail(error, "cannot create restore destination", ec);
    fs::path temporary = target; temporary += ".restore.tmp";
    fs::remove(temporary, ec); ec.clear();
    if (!copy_flushed(backup, temporary, error)) return false;
    if (fs::exists(target, ec) && !backup_save(folder / "nand", target, error)) { fs::remove(temporary, ec); return false; }
    fs::rename(temporary, target, ec);
#if defined(_WIN32)
    // A restarted child can reach this before the old process has finished closing its handles.
    for (int attempt = 0; ec && attempt < 25; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        fs::rename(temporary, target, ec);
    }
#endif
    if (ec) { const auto reason = ec; fs::remove(temporary, ec); return fail(error, "cannot restore save", reason); }
    fs::remove(request, ec);
    if (ec) return fail(error, "save restored but restore request cannot be removed", ec);
    error.clear(); return true;
}
}  // namespace punchout
