// Where this port keeps what it writes (paths.hpp).
#include "qilin/host/paths.hpp"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <system_error>

#include "qilin/host/vulkan.hpp"

namespace qilin::host {

std::filesystem::path user_folder() {
    std::filesystem::path p;
    if (const char* env = std::getenv("PUNCHOUT_SAVE_DIR"); env && *env) {
        p = env;
    } else {
        char* pref = SDL_GetPrefPath("Qilin", "PunchOut-QilinComp");
        if (!pref) die("can't find a folder for saves: %s", SDL_GetError());
        p = std::filesystem::path(reinterpret_cast<const char8_t*>(pref));  // SDL paths are UTF-8
        SDL_free(pref);
    }
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    if (ec) die("can't create %s: %s", p.string().c_str(), ec.message().c_str());
    return p;
}

}  // namespace qilin::host
