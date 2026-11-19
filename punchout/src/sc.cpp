// The console settings (revolution/sc) this port's Wii reports: the language,
// the aspect ratio and the sound from the settings file's [console]
// (settings.hpp, set before the game starts); the rest fixed, as a European
// console would hold them.
#include "boot.hpp"

namespace punchout {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

uint32_t g_aspect = 1;    // SC_ASPECT_RATIO_16x9 (1), the game's widescreen mode, or 4x3 (0)
uint32_t g_language = 1;  // SC_LANG_EN (1); the PAL disc also has DE (2), FR (3), ES (4), IT (5)
uint32_t g_sound = 1;     // SC_SOUND_MODE_STEREO (1) or MONO (0)
constexpr uint32_t kEuRgb60 = 1;      // 60 Hz (vi.cpp runs the retrace at 60)
constexpr uint32_t kProgressive = 0;  // no component cable (VIGetDTVStatus says 0)
constexpr uint32_t kAreaEurope = 2;   // SC_PRODUCT_AREA_EUR

void SCInit(Cpu&) {}
void SCCheckStatus(Cpu& c) { c.r[3] = 0; }  // SC_STATUS_READY
void SCGetAspectRatio(Cpu& c) { c.r[3] = g_aspect; }
void SCGetEuRgb60Mode(Cpu& c) { c.r[3] = kEuRgb60; }
void SCGetLanguage(Cpu& c) { c.r[3] = g_language; }
void SCGetProgressiveMode(Cpu& c) { c.r[3] = kProgressive; }
void SCGetSoundMode(Cpu& c) { c.r[3] = g_sound; }
void SCGetProductArea(Cpu& c) { c.r[3] = kAreaEurope; }

const rt::Provided kReplacements[] = {
    {0x80258968u, "SCInit", SCInit},
    {0x80258A08u, "SCCheckStatus", SCCheckStatus},
    {0x8025A2CCu, "SCGetAspectRatio", SCGetAspectRatio},
    {0x8025A394u, "SCGetEuRgb60Mode", SCGetEuRgb60Mode},
    {0x8025A3F4u, "SCGetLanguage", SCGetLanguage},
    {0x8025A460u, "SCGetProgressiveMode", SCGetProgressiveMode},
    {0x8025A508u, "SCGetSoundMode", SCGetSoundMode},
    {0x8025A8BCu, "SCGetProductArea", SCGetProductArea},
};

}  // namespace

void set_console(unsigned language, bool widescreen, unsigned sound) {
    g_language = language, g_aspect = widescreen ? 1 : 0, g_sound = sound;
}

std::span<const rt::Provided> sc_replacements() { return kReplacements; }

}  // namespace punchout
