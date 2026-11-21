// Bounded save history, stored outside the emulated NAND.
#pragma once
#include <filesystem>
#include <string>
#include <vector>
namespace punchout {
struct SaveBackup { std::filesystem::path file; std::string label; };
// Keep the five most recent pre-write copies per NAND file.
bool backup_save(const std::filesystem::path& nand, const std::filesystem::path& file, std::string& error);
std::vector<SaveBackup> save_backups(const std::filesystem::path& folder);
// A restore is applied on the next start, before any guest file is opened.
bool request_save_restore(const std::filesystem::path& folder, const std::filesystem::path& backup, std::string& error);
bool apply_save_restore(const std::filesystem::path& folder, std::string& error);
}  // namespace punchout
