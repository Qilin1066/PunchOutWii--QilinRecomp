// The Wii Remote library's replacements (revolution/wpad): one sideways Wii
// Remote on channel 0, buttons only, from the player's pad or keyboard.
//
// The decision for this port: buttons, no motion controls, no Nunchuk, no
// Balance Board (docs/roadmap-and-design-targets.md, "Audio/Input"). So the
// remote reports itself as a bare core controller, resting flat, with nothing
// in view of its pointer, and the other three channels as empty. KPAD and WBC
// stay lifted and see exactly that.
//
// The mapping (to be remappable in the settings file): held sideways, the
// remote's cross pad is turned a quarter: the player's up is its RIGHT, left
// its UP, down its LEFT, right its DOWN.
//   pad D-pad or left stick -> cross pad (dodge, duck, block)
//   A -> 2 (right punch), B -> 1 (left punch), Y or X -> A (Star Punch)
//   Start -> +, View -> -, Guide -> HOME
#include "boot.hpp"
#include "input.hpp"
#include "os.hpp"
#include "qilin/host/display.hpp"

namespace punchout {
namespace rt = qilin::rt;
using rt::Cpu;
namespace host = qilin::host;

namespace {

// WPAD's numbers (wpad.h).
constexpr uint32_t kErrNone = 0, kErrNoController = 0xFFFFFFFFu;  // WPAD_ERR_NONE, WPAD_ERR_NO_CONTROLLER (-1)
constexpr uint32_t kDevCore = 0, kDevNotFound = 253;
constexpr uint32_t kStateSetup = 3;
enum : uint16_t {
    kLeft = 0x0001, kRight = 0x0002, kDown = 0x0004, kUp = 0x0008, kPlus = 0x0010,
    kTwo = 0x0100, kOne = 0x0200, kB = 0x0400, kA = 0x0800, kMinus = 0x1000, kHome = 0x8000,
};
constexpr int16_t kOneG = 104;  // the accelerometer's 1 g, as Dolphin's default calibration gives it
constexpr uint64_t kSampleTicks = kTimerHz / 200;  // the remote reports at 200 Hz

uint32_t g_connect_cb[4] = {}, g_extension_cb[4] = {}, g_sampling_cb[4] = {};
uint32_t g_format[4] = {};
bool g_connected = false;  // channel 0, once its connect callback has run
bool g_motor_enabled = true, g_dpd[4] = {}, g_speaker[4] = {};
uint8_t g_speaker_volume = 0x40;
uint64_t g_next_sample = 0;

uint16_t wii_buttons() {
    const uint32_t p = g_pad_buttons.load(std::memory_order_relaxed);
    uint16_t b = 0;
    if (p & host::kPadUp) b |= kRight;
    if (p & host::kPadLeft) b |= kUp;
    if (p & host::kPadDown) b |= kLeft;
    if (p & host::kPadRight) b |= kDown;
    if (p & host::kPadA) b |= kTwo;
    if (p & host::kPadB) b |= kOne;
    if (p & (host::kPadX | host::kPadY)) b |= kA;
    if (p & host::kPadStart) b |= kPlus;
    if (p & host::kPadBack) b |= kMinus;
    if (p & host::kPadGuide) b |= kHome;
    return b;
}

// Run a guest callback (chan, result) as the Bluetooth stack's interrupt would.
void later(uint32_t cb, uint32_t a0, uint32_t a1) {
    if (cb) post_interrupt([cb, a0, a1](Cpu& c) { guest_callback(c, cb, a0, a1); });
}

void poll_samples() {
    if (!g_connected || !g_sampling_cb[0]) return;
    const uint64_t now = time_base();
    if (g_next_sample == 0) g_next_sample = now;
    while (now >= g_next_sample) {
        g_next_sample += kSampleTicks;
        later(g_sampling_cb[0], 0, 0);
    }
}

// ---- set-up
void WPADInit(Cpu&) {
    static bool done = false;
    if (done) return;
    done = true;
    add_timer_source(poll_samples, [] { return g_connected && g_sampling_cb[0] ? g_next_sample : UINT64_MAX; });
}
void WPADGetStatus(Cpu& c) { c.r[3] = kStateSetup; }
void returns_zero(Cpu& c) { c.r[3] = 0; }
void nothing(Cpu&) {}
uint32_t g_simple_sync_cb = 0;
void WPADSetSimpleSyncCallback(Cpu& c) {
    const uint32_t was = g_simple_sync_cb;
    g_simple_sync_cb = c.r[3];
    c.r[3] = was;
}
void WPADGetRadioSensitivity(Cpu& c) { c.r[3] = c.r[3] == 0 ? 100 : 0; }  // (chan): a strong signal
void WPADGetSensorBarPosition(Cpu& c) { c.r[3] = 0; }  // below the TV

// ---- connection
void WPADSetConnectCallback(Cpu& c) {  // (chan, cb) -> the previous one
    const uint32_t ch = c.r[3] & 3, was = g_connect_cb[ch];
    g_connect_cb[ch] = c.r[4];
    c.r[3] = was;
    if (ch == 0 && g_connect_cb[0]) {  // the remote is already on: it connects
        post_interrupt([](Cpu& cc) {
            g_connected = true;
            if (g_connect_cb[0]) guest_callback(cc, g_connect_cb[0], 0, kErrNone);
            if (g_extension_cb[0]) guest_callback(cc, g_extension_cb[0], 0, kDevCore);
        });
    }
}
void WPADSetExtensionCallback(Cpu& c) {
    const uint32_t ch = c.r[3] & 3, was = g_extension_cb[ch];
    g_extension_cb[ch] = c.r[4];
    c.r[3] = was;
}
void WPADSetSamplingCallback(Cpu& c) {
    const uint32_t ch = c.r[3] & 3, was = g_sampling_cb[ch];
    g_sampling_cb[ch] = c.r[4];
    c.r[3] = was;
}
void WPADProbe(Cpu& c) {  // (chan, u32* type) -> error
    const bool here = c.r[3] == 0;
    if (c.r[4]) rt::poke32(c.r[4], here ? kDevCore : kDevNotFound);
    c.r[3] = here ? kErrNone : kErrNoController;
}
void WPADDisconnect(Cpu& c) { c.r[3] = c.r[3] == 0 ? kErrNone : kErrNoController; }
void WPADGetDataFormat(Cpu& c) { c.r[3] = g_format[c.r[3] & 3]; }
void WPADSetDataFormat(Cpu& c) {
    g_format[c.r[3] & 3] = c.r[4];
    c.r[3] = c.r[3] == 0 ? kErrNone : kErrNoController;
}
void WPADGetInfoAsync(Cpu& c) {  // (chan, WPADInfo*, cb) -> error
    const uint32_t ch = c.r[3], info = c.r[4], cb = c.r[5];
    if (ch != 0) {
        c.r[3] = kErrNoController;
        return;
    }
    rt::poke32(info + 0, g_dpd[0]);
    rt::poke32(info + 4, g_speaker[0]);
    rt::poke32(info + 8, 0);   // nothing attached
    rt::poke32(info + 12, 0);  // battery not low
    rt::poke32(info + 16, 0);
    rt::poke8(info + 20, 4);   // battery: full
    rt::poke8(info + 21, 1);   // LED 1
    later(cb, 0, kErrNone);
    c.r[3] = kErrNone;
}
void WPADGetAccGravityUnit(Cpu& c) {  // (chan, type, WPADAcc*): 1 g on each axis
    if (c.r[5]) {
        rt::poke16(c.r[5], kOneG);
        rt::poke16(c.r[5] + 2, kOneG);
        rt::poke16(c.r[5] + 4, kOneG);
    }
}

// ---- reading: a WPADStatus is buttons (+0), acceleration x, y, z (+2), four
// pointer objects of 8 bytes (+8), the device type (+0x28) and an error (+0x29).
void WPADRead(Cpu& c) {
    const uint32_t ch = c.r[3], s = c.r[4];
    std::memset(rt::guest_ptr(s), 0, 0x2A);
    if (ch == 0 && g_connected) {
        rt::poke16(s, wii_buttons());
        rt::poke16(s + 6, static_cast<uint16_t>(kOneG));  // resting flat: 1 g on z
        rt::poke8(s + 0x28, kDevCore);
    } else {
        rt::poke8(s + 0x28, kDevNotFound);
        rt::poke8(s + 0x29, 0xFF);  // WPAD_ERR_NO_CONTROLLER
    }
}
void WPADIsUsedCallbackByKPAD(Cpu& c) { c.r[3] = 0; }

// ---- rumble
void WPADControlMotor(Cpu& c) {  // (chan, cmd): 1 on, 0 off
    if (c.r[3] == 0) g_rumble.store(g_motor_enabled && c.r[4] == 1, std::memory_order_relaxed);
}
void WPADEnableMotor(Cpu& c) {
    g_motor_enabled = c.r[3] != 0;
    if (!g_motor_enabled) g_rumble.store(false, std::memory_order_relaxed);
}
void WPADIsMotorEnabled(Cpu& c) { c.r[3] = g_motor_enabled; }
void WPADSaveConfig(Cpu& c) {  // (cb) -> BOOL
    later(c.r[3], 0, 0);
    c.r[3] = 1;
}

// ---- the speaker: accepted and dropped (to become a host sound source)
void WPADIsSpeakerEnabled(Cpu& c) { c.r[3] = g_speaker[c.r[3] & 3]; }
void WPADControlSpeaker(Cpu& c) {  // (chan, cmd, cb): 0 off, 1 on, others mute/play/...
    const uint32_t ch = c.r[3];
    if (ch != 0) {
        c.r[3] = kErrNoController;
        return;
    }
    if (c.r[4] == 0) g_speaker[0] = false;
    if (c.r[4] == 1) g_speaker[0] = true;
    later(c.r[5], 0, kErrNone);
    c.r[3] = kErrNone;
}
void WPADGetSpeakerVolume(Cpu& c) { c.r[3] = g_speaker_volume; }
void WPADSetSpeakerVolume(Cpu& c) { g_speaker_volume = static_cast<uint8_t>(c.r[3]); }
void WPADCanSendStreamData(Cpu& c) { c.r[3] = c.r[3] == 0; }
void WPADSendStreamData(Cpu& c) { c.r[3] = c.r[3] == 0 ? kErrNone : kErrNoController; }

// ---- the pointer: never anything in view
void WPADGetDpdSensitivity(Cpu& c) { c.r[3] = 3; }
void WPADIsDpdEnabled(Cpu& c) { c.r[3] = g_dpd[c.r[3] & 3]; }
void WPADGetDpdFormat(Cpu& c) { c.r[3] = 0; }
void WPADControlDpd(Cpu& c) {  // (chan, cmd, cb)
    const uint32_t ch = c.r[3];
    if (ch != 0) {
        c.r[3] = kErrNoController;
        return;
    }
    g_dpd[0] = c.r[4] != 0;
    later(c.r[5], 0, kErrNone);
    c.r[3] = kErrNone;
}

// ---- the Balance Board and extensions: none
void no_controller(Cpu& c) { c.r[3] = kErrNoController; }

const rt::Provided kReplacements[] = {
    {0x802188D4u, "WPADInit", WPADInit},
    // One-instruction branches into WUD, the Bluetooth device layer (their targets name them).
    {0x80218944u, "WPADStartSimpleSync", returns_zero},
    {0x80218948u, "WPADStopSimpleSync", returns_zero},
    {0x8021894Cu, "WPADSetSimpleSyncCallback", WPADSetSimpleSyncCallback},
    {0x80218950u, "WPADRegisterAllocator", nothing},
    {0x80218954u, "WPADGetStatus", WPADGetStatus},
    {0x80218958u, "WPADGetRadioSensitivity", WPADGetRadioSensitivity},
    {0x8021899Cu, "WPADGetSensorBarPosition", WPADGetSensorBarPosition},
    {0x80219A94u, "WPADGetAccGravityUnit", WPADGetAccGravityUnit},
    {0x80219B90u, "WPADDisconnect", WPADDisconnect},
    {0x80219C50u, "WPADProbe", WPADProbe},
    {0x80219CDCu, "WPADSetSamplingCallback", WPADSetSamplingCallback},
    {0x80219D64u, "WPADSetConnectCallback", WPADSetConnectCallback},
    {0x80219DECu, "WPADSetExtensionCallback", WPADSetExtensionCallback},
    {0x80219E48u, "WPADGetDataFormat", WPADGetDataFormat},
    {0x80219E8Cu, "WPADSetDataFormat", WPADSetDataFormat},
    {0x80219F98u, "WPADGetInfoAsync", WPADGetInfoAsync},
    {0x8021A1B8u, "WPADControlMotor", WPADControlMotor},
    {0x8021A284u, "WPADEnableMotor", WPADEnableMotor},
    {0x8021A2B8u, "WPADIsMotorEnabled", WPADIsMotorEnabled},
    {0x8021A4D4u, "WPADSaveConfig", WPADSaveConfig},
    {0x8021A580u, "WPADRead", WPADRead},
    {0x8021A9D8u, "WPADIsSpeakerEnabled", WPADIsSpeakerEnabled},
    {0x8021AA1Cu, "WPADControlSpeaker", WPADControlSpeaker},
    {0x8021C038u, "WPADGetSpeakerVolume", WPADGetSpeakerVolume},
    {0x8021C06Cu, "WPADSetSpeakerVolume", WPADSetSpeakerVolume},
    {0x8021C1F4u, "WPADCanSendStreamData", WPADCanSendStreamData},
    {0x8021C278u, "WPADSendStreamData", WPADSendStreamData},
    {0x8021C488u, "WPADGetDpdSensitivity", WPADGetDpdSensitivity},
    {0x8021C4BCu, "WPADIsDpdEnabled", WPADIsDpdEnabled},
    {0x8021C500u, "WPADGetDpdFormat", WPADGetDpdFormat},
    {0x8021C590u, "WPADControlDpd", WPADControlDpd},
    {0x8021D610u, "WPADControlBLC (Balance Board)", no_controller},
    {0x8021D824u, "WPADWriteExtReg", no_controller},
    {0x8021D8BCu, "WPADReadExtReg", no_controller},
    {0x8021E138u, "WPADIsUsedCallbackByKPAD", WPADIsUsedCallbackByKPAD},
};

}  // namespace

std::span<const rt::Provided> wpad_replacements() { return kReplacements; }

}  // namespace punchout
