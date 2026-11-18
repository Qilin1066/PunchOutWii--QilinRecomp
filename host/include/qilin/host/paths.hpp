// Where this port keeps what it writes (saves, settings).
//
// Runtime code: no std::format (macOS 10.15).
#pragma once

#include <filesystem>

namespace qilin::host {

// The per-user folder for this port: PUNCHOUT_SAVE_DIR if set, otherwise the
// platform's (Windows: %APPDATA%\Qilin\PunchOut-QilinComp; macOS: ~/Library/
// Application Support/...; Linux: ~/.local/share/...). Created if missing.
std::filesystem::path user_folder();

}  // namespace qilin::host
