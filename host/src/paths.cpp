// Where this port keeps what it writes (paths.hpp).
#include "qilin/host/paths.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <system_error>

#include "qilin/host/vulkan.hpp"

namespace qilin::host {

namespace {
std::filesystem::path user_data_base() {
#if defined(_WIN32)
    if (const char* p = std::getenv("LOCALAPPDATA"); p && *p) return p;
#elif defined(__APPLE__)
    if (const char* p = std::getenv("HOME"); p && *p) return std::filesystem::path(p) / "Library" / "Application Support";
#else
    if (const char* p = std::getenv("XDG_DATA_HOME"); p && *p) return p;
    if (const char* p = std::getenv("HOME"); p && *p) return std::filesystem::path(p) / ".local" / "share";
#endif
    return {};
}

// v0.17.0's folder, where it is (without creating it).
std::filesystem::path old_folder() {
    char* pref = SDL_GetPrefPath("Qilin", "PunchOut-QilinComp");  // this creates it, if missing: removed below when empty
    if (!pref) return {};
    std::filesystem::path p(reinterpret_cast<const char8_t*>(pref));  // SDL paths are UTF-8
    SDL_free(pref);
    return p;
}
}  // namespace

std::filesystem::path user_folder() {
    static const std::filesystem::path folder = [] {
        std::filesystem::path p;
        std::error_code ec;
        if (const char* env = std::getenv("PUNCHOUT_SAVE_DIR"); env && *env) {
            p = env;
        } else {
            const std::filesystem::path base = user_data_base();
            if (base.empty()) die("can't find a folder for saves: no LOCALAPPDATA, HOME or XDG_DATA_HOME");
            p = base / "qilin" / "punchout";
            const std::filesystem::path before = old_folder();
            if (!before.empty() && std::filesystem::is_directory(before, ec)) {
                if (std::filesystem::is_empty(before, ec)) {
                    std::filesystem::remove(before, ec);  // only SDL's own making, and its parent if that's empty too
                    if (std::filesystem::is_empty(before.parent_path(), ec)) std::filesystem::remove(before.parent_path(), ec);
                } else if (!std::filesystem::exists(p, ec)) {
                    std::filesystem::create_directories(p.parent_path(), ec);
                    std::filesystem::rename(before, p, ec);
                    if (ec)
                        std::fprintf(stderr, "punchout: can't move %s to %s (%s): your saves are still in the old place\n",
                                     before.string().c_str(), p.string().c_str(), ec.message().c_str());
                    else
                        std::fprintf(stderr, "punchout: moved %s to %s\n", before.string().c_str(), p.string().c_str());
                    ec.clear();
                    if (std::filesystem::is_empty(before.parent_path(), ec)) std::filesystem::remove(before.parent_path(), ec);
                    ec.clear();
                }
            }
        }
        std::filesystem::create_directories(p, ec);
        if (ec) die("can't create %s: %s", p.string().c_str(), ec.message().c_str());
        return p;
    }();
    return folder;
}

}  // namespace qilin::host
