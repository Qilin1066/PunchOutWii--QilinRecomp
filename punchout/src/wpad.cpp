// The Wii Remote library's replacements (revolution/wpad): a sideways Wii
// Remote on channel 0, buttons only, from the player's pad or keyboard, and a
// second on channel 1 while a second player's controller is there (input.hpp,
// g_player2: Head-to-Head).
//
// The decision for this port: buttons, no motion controls, no Nunchuk, no
// Balance Board (docs/roadmap-and-design-targets.md, "Audio/Input"). So a
// remote reports itself as a bare core controller, resting flat; the first
// points where the mouse or the right stick does, the second where its own
// controller's right stick does, and the other channels are empty. KPAD and
// WBC stay lifted and see exactly that.
//
// The mapping is the player's, from settings.ini (settings.hpp); by default
//   pad D-pad or left stick -> cross pad (dodge, duck, block)
//   A -> 2 (right punch), B -> 1 (left punch), Y, X or the mouse -> A (Star Punch)
//   Start -> +, View -> -, Guide -> HOME
// Held sideways, the remote's cross pad is turned a quarter: the player's up
// is its RIGHT, left its UP, down its LEFT, right its DOWN.
#include <algorithm>

#include "boot.hpp"
#include "input.hpp"
#include "os.hpp"
#include "settings.hpp"
#include "speaker.hpp"

namespace punchout {
namespace rt = qilin::rt;
using rt::Cpu;

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
constexpr uint32_t kRemotes = 2;
bool g_connected[kRemotes] = {};  // once the channel's connect callback has run
bool g_motor_enabled = true, g_dpd[4] = {}, g_speaker[4] = {};
uint8_t g_speaker_volume = 0x40;
uint64_t g_next_sample[kRemotes] = {};
uint64_t g_next_look = 0;  // when to look again for the second player's controller coming or going

bool here(uint32_t ch) { return ch < kRemotes && g_connected[ch]; }

uint16_t wii_buttons(uint32_t ch) {
    static constexpr uint16_t kBits[kControlCount] = {
        kRight, kLeft, kUp, kDown,  // the player's up, down, left, right: the cross pad turned a quarter
        kOne, kTwo, kA, kPlus, kMinus, kHome,
    };
    const uint32_t held = (ch == 0 ? g_controls : g_controls2).load(std::memory_order_relaxed);
    uint16_t b = 0;
    for (unsigned i = 0; i < kControlCount; ++i)
        if (held & (1u << i)) b |= kBits[i];
    return b;
}

// Run a guest callback (chan, result) as the Bluetooth stack's interrupt would.
void later(uint32_t cb, uint32_t a0, uint32_t a1) {
    if (cb) post_interrupt([cb, a0, a1](Cpu& c) { guest_callback(c, cb, a0, a1); });
}

// A remote comes on: its connect callback, then its extension callback (none attached). Also
// when a callback is set on a remote that is already on: the console's remotes take seconds to
// connect, after everything has set its callback, so each one set learns of the remote.
void connect(uint32_t ch) {
    post_interrupt([ch](Cpu& c) {
        if (!g_connected[ch]) g_next_sample[ch] = 0;
        g_connected[ch] = true;
        if (g_connect_cb[ch]) guest_callback(c, g_connect_cb[ch], ch, kErrNone);
        if (g_extension_cb[ch]) guest_callback(c, g_extension_cb[ch], ch, kDevCore);
    });
}
void disconnect(uint32_t ch) {
    post_interrupt([ch](Cpu& c) {
        if (!g_connected[ch]) return;
        g_connected[ch] = false;
        g_dpd[ch] = g_speaker[ch] = false;
        if (g_connect_cb[ch]) guest_callback(c, g_connect_cb[ch], ch, kErrNoController);
    });
}

void poll_remotes() {
    const uint64_t now = time_base();
    // The second player's controller, coming or going (looked for 20 times a second).
    if (now >= g_next_look) {
        g_next_look = now + kTimerHz / 20;
        const bool there = g_player2.load(std::memory_order_relaxed);
        if (there && !g_connected[1] && g_connect_cb[1]) connect(1);
        if (!there && g_connected[1]) {
            g_rumble2.store(false, std::memory_order_relaxed);
            disconnect(1);
        }
    }
    for (uint32_t ch = 0; ch < kRemotes; ++ch) {
        if (!g_connected[ch] || !g_sampling_cb[ch]) continue;
        if (g_next_sample[ch] == 0) g_next_sample[ch] = now;
        while (now >= g_next_sample[ch]) {
            g_next_sample[ch] += kSampleTicks;
            later(g_sampling_cb[ch], ch, 0);
        }
    }
}
uint64_t remotes_deadline() {
    uint64_t t = g_next_look;
    for (uint32_t ch = 0; ch < kRemotes; ++ch)
        if (g_connected[ch] && g_sampling_cb[ch]) t = std::min(t, g_next_sample[ch]);
    return t;
}

// ---- set-up
void WPADInit(Cpu&) {
    static bool done = false;
    if (done) return;
    done = true;
    add_timer_source(poll_remotes, remotes_deadline);
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
void WPADGetRadioSensitivity(Cpu& c) { c.r[3] = here(c.r[3]) ? 100 : 0; }  // (chan): a strong signal
void WPADGetSensorBarPosition(Cpu& c) { c.r[3] = 0; }  // below the TV

// ---- connection
void WPADSetConnectCallback(Cpu& c) {  // (chan, cb) -> the previous one
    const uint32_t ch = c.r[3] & 3, was = g_connect_cb[ch];
    g_connect_cb[ch] = c.r[4];
    c.r[3] = was;
    // A remote that is already on connects: the first always is, the second while its player's controller is there.
    if (g_connect_cb[ch] && (ch == 0 || (ch == 1 && g_player2.load(std::memory_order_relaxed)))) connect(ch);
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
    const bool on = here(c.r[3]);
    if (c.r[4]) rt::poke32(c.r[4], on ? kDevCore : kDevNotFound);
    c.r[3] = on ? kErrNone : kErrNoController;
}
void WPADDisconnect(Cpu& c) { c.r[3] = here(c.r[3]) ? kErrNone : kErrNoController; }  // (the player's controller stays: so does the remote)
void WPADGetDataFormat(Cpu& c) { c.r[3] = g_format[c.r[3] & 3]; }
void WPADSetDataFormat(Cpu& c) {
    g_format[c.r[3] & 3] = c.r[4];
    c.r[3] = here(c.r[3]) ? kErrNone : kErrNoController;
}
void WPADGetInfoAsync(Cpu& c) {  // (chan, WPADInfo*, cb) -> error
    const uint32_t ch = c.r[3], info = c.r[4], cb = c.r[5];
    if (!here(ch)) {
        c.r[3] = kErrNoController;
        return;
    }
    rt::poke32(info + 0, g_dpd[ch]);
    rt::poke32(info + 4, g_speaker[ch]);
    rt::poke32(info + 8, 0);   // nothing attached
    rt::poke32(info + 12, 0);  // battery not low
    rt::poke32(info + 16, 0);
    rt::poke8(info + 20, 4);   // battery: full
    rt::poke8(info + 21, static_cast<uint8_t>(1u << ch));  // LED 1, or LED 2
    later(cb, ch, kErrNone);
    c.r[3] = kErrNone;
}
void WPADGetAccGravityUnit(Cpu& c) {  // (chan, type, WPADAcc*): 1 g on each axis
    if (c.r[5]) {
        rt::poke16(c.r[5], kOneG);
        rt::poke16(c.r[5] + 2, kOneG);
        rt::poke16(c.r[5] + 4, kOneG);
    }
}

// ---- the pointer: the two lights of the sensor bar as the remote's camera
// sees them when it points where the player points. The geometry is
// Dolphin's camera emulation (WiimoteEmu/Camera.cpp): a 1024x768 camera with
// a 42 by 31.5 degree view, the bar's lights 20 cm apart, seen from 2 m; the
// remote turns to point. Each object is x, y (camera pixels), a size and a trace id.
void write_dots(uint32_t at, uint32_t ch) {
    if (ch == 0 && !g_pointer_on.load(std::memory_order_relaxed)) return;  // pointing off the screen: no lights
    constexpr double kPi = 3.14159265358979323846, kDeg = kPi / 180.0;
    constexpr double kDistance = 2.0, kHalfSeparation = 0.1;
    const double px = (ch == 0 ? g_pointer_x : g_pointer2_x).load(std::memory_order_relaxed) / 1000.0,
                 py = (ch == 0 ? g_pointer_y : g_pointer2_y).load(std::memory_order_relaxed) / 1000.0;
    // Measured against the game's cursor: 8.7 degrees either way spans the
    // screen, and with the bar below the TV (the console's default) the
    // remote points 4.5 degrees up to reach the middle.
    const double yaw = px * 8.7 * kDeg, pitch = (py * 8.7 + 4.5) * kDeg;
    const double tx = std::tan(21.0 * kDeg), ty = std::tan(15.75 * kDeg);
    for (int i = 0; i < 2; ++i) {
        const double led = i == 0 ? -kHalfSeparation : kHalfSeparation;
        const double a = std::atan2(led, kDistance) + yaw, b = -pitch;
        const double x = (1.0 - std::tan(a) / tx) * 512.0, y = (1.0 - std::tan(b) / ty) * 384.0;
        if (x < 0 || x >= 1024 || y < 0 || y >= 768) continue;
        rt::poke16(at + 8 * i, static_cast<uint16_t>(std::lround(x)));
        rt::poke16(at + 8 * i + 2, static_cast<uint16_t>(std::lround(y)));
        rt::poke16(at + 8 * i + 4, 2);  // size, as at 2 m
        rt::poke8(at + 8 * i + 6, static_cast<uint8_t>(i));
    }
}

// ---- reading: a WPADStatus is buttons (+0), acceleration x, y, z (+2), four
// pointer objects of 8 bytes (+8), the device type (+0x28) and an error (+0x29).
void WPADRead(Cpu& c) {
    const uint32_t ch = c.r[3], s = c.r[4];
    std::memset(rt::guest_ptr(s), 0, 0x2A);
    if (here(ch)) {
        rt::poke16(s, wii_buttons(ch));
        rt::poke16(s + 6, static_cast<uint16_t>(kOneG));  // resting flat: 1 g on z
        if (g_dpd[ch]) write_dots(s + 8, ch);
        rt::poke8(s + 0x28, kDevCore);
    } else {
        rt::poke8(s + 0x28, kDevNotFound);
        rt::poke8(s + 0x29, 0xFF);  // WPAD_ERR_NO_CONTROLLER
    }
}
void WPADIsUsedCallbackByKPAD(Cpu& c) { c.r[3] = 0; }

// ---- rumble
void WPADControlMotor(Cpu& c) {  // (chan, cmd): 1 on, 0 off
    if (here(c.r[3])) (c.r[3] == 0 ? g_rumble : g_rumble2).store(g_motor_enabled && c.r[4] == 1, std::memory_order_relaxed);
}
void WPADEnableMotor(Cpu& c) {
    g_motor_enabled = c.r[3] != 0;
    if (!g_motor_enabled) g_rumble.store(false, std::memory_order_relaxed), g_rumble2.store(false, std::memory_order_relaxed);
}
void WPADIsMotorEnabled(Cpu& c) { c.r[3] = g_motor_enabled; }
void WPADSaveConfig(Cpu& c) {  // (cb) -> BOOL
    later(c.r[3], 0, 0);
    c.r[3] = 1;
}

// ---- the speaker: its sound is decoded and mixed into the PC's (speaker.hpp)
void WPADIsSpeakerEnabled(Cpu& c) { c.r[3] = g_speaker[c.r[3] & 3]; }
void WPADControlSpeaker(Cpu& c) {  // (chan, cmd, cb): 0 off, 1 on, others mute/play/...
    const uint32_t ch = c.r[3];
    if (!here(ch)) {
        c.r[3] = kErrNoController;
        return;
    }
    // WPAD's commands: 0 off, 1 on, 2 mute, 3 unmute, 4 play, 5 (a register write). On, the remote's
    // decoder starts afresh, as the game's encoder does with the first block it sends after.
    switch (c.r[4]) {
    case 0: g_speaker[ch] = false, g_speakers[ch].reset(); break;
    case 1: g_speaker[ch] = true, g_speakers[ch].reset(); break;
    case 2: g_speakers[ch].set_muted(true); break;
    case 3: g_speakers[ch].set_muted(false); break;
    default: break;
    }
    later(c.r[5], ch, kErrNone);
    c.r[3] = kErrNone;
}
void WPADGetSpeakerVolume(Cpu& c) { c.r[3] = g_speaker_volume; }
void WPADSetSpeakerVolume(Cpu& c) {
    g_speaker_volume = static_cast<uint8_t>(c.r[3]);
    for (Speaker& s : g_speakers) s.set_volume(g_speaker_volume);
}
void WPADCanSendStreamData(Cpu& c) { c.r[3] = here(c.r[3]); }
void WPADSendStreamData(Cpu& c) {  // (chan, data, length): 20 bytes of 4-bit ADPCM, 40 samples at 6 kHz
    const uint32_t ch = c.r[3], n = c.r[5];
    if (!here(ch)) {
        c.r[3] = kErrNoController;
        return;
    }
    if (n > 64) rt::fatal(&c, "WPADSendStreamData(%u, %08X, %u): more than a report holds", ch, c.r[4], n);
    if (g_speaker[ch]) g_speakers[ch].send(rt::guest_ptr(c.r[4]), n);
    c.r[3] = kErrNone;
}
// ---- the pointer's camera
void WPADGetDpdSensitivity(Cpu& c) { c.r[3] = 3; }
void WPADIsDpdEnabled(Cpu& c) { c.r[3] = g_dpd[c.r[3] & 3]; }
void WPADGetDpdFormat(Cpu& c) { c.r[3] = 0; }
void WPADControlDpd(Cpu& c) {  // (chan, cmd, cb)
    const uint32_t ch = c.r[3];
    if (!here(ch)) {
        c.r[3] = kErrNoController;
        return;
    }
    g_dpd[ch] = c.r[4] != 0;
    later(c.r[5], ch, kErrNone);
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
