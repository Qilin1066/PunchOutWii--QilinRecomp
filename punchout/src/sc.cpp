// The console settings (revolution/sc) this port's Wii reports. They'll come
// from the settings file (docs/roadmap-and-design-targets.md, "Settings");
// until then, fixed values a European console would plausibly hold.
#include "boot.hpp"

namespace punchout {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

constexpr uint32_t kAspect16x9 = 1;       // SC_ASPECT_RATIO_16x9: the game's own widescreen mode
constexpr uint32_t kEuRgb60 = 1;          // 60 Hz (vi.cpp runs the retrace at 60)
constexpr uint32_t kLanguageEnglish = 1;  // SC_LANG_EN; the PAL disc also has FR, DE, ES, IT
constexpr uint32_t kProgressive = 0;      // no component cable (VIGetDTVStatus says 0)
constexpr uint32_t kSoundStereo = 1;
constexpr uint32_t kAreaEurope = 2;       // SC_PRODUCT_AREA_EUR

void SCInit(Cpu&) {}
void SCCheckStatus(Cpu& c) { c.r[3] = 0; }  // SC_STATUS_READY
void SCGetAspectRatio(Cpu& c) { c.r[3] = kAspect16x9; }
void SCGetEuRgb60Mode(Cpu& c) { c.r[3] = kEuRgb60; }
void SCGetLanguage(Cpu& c) { c.r[3] = kLanguageEnglish; }
void SCGetProgressiveMode(Cpu& c) { c.r[3] = kProgressive; }
void SCGetSoundMode(Cpu& c) { c.r[3] = kSoundStereo; }
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

std::span<const rt::Provided> sc_replacements() { return kReplacements; }

}  // namespace punchout
