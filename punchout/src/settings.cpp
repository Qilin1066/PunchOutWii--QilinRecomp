#include "settings.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

#include "qilin/host/pad.hpp"
#include "qilin/rt/system.hpp"
#include "qilin/rt/file.hpp"
#include <set>

namespace punchout {
namespace host = qilin::host;

namespace {

constexpr const char* kControlNames[kControlCount] = {"up", "down", "left", "right", "one",
                                                      "two", "a", "plus", "minus", "home"};
constexpr const char* kControlLabels[kControlCount] = {
    "Up (block)", "Down (duck)", "Left (dodge)", "Right (dodge)", "1 (left punch)",
    "2 (right punch)", "A (Star Punch, OK)", "+ (pause)", "- (back)", "Home"};
struct PadName {
    const char* name;   // in the file
    const char* label;  // in the menu
    uint32_t bit;
};
constexpr PadName kPadNames[] = {
    {"Up", "Pad Up", host::kPadUp},       {"Down", "Pad Down", host::kPadDown},  {"Left", "Pad Left", host::kPadLeft},
    {"Right", "Pad Right", host::kPadRight}, {"A", "Pad A", host::kPadA},           {"B", "Pad B", host::kPadB},
    {"X", "Pad X", host::kPadX},          {"Y", "Pad Y", host::kPadY},           {"Start", "Pad Start", host::kPadStart},
    {"Back", "Pad Back", host::kPadBack}, {"Guide", "Pad Guide", host::kPadGuide},
    {"LB", "Pad LB", host::kPadLB},       {"RB", "Pad RB", host::kPadRB},        {"LT", "Pad LT", host::kPadLT},
    {"RT", "Pad RT", host::kPadRT},       {"L3", "Pad L3", host::kPadL3},        {"R3", "Pad R3", host::kPadR3},
};

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool parse_binding(const std::string& text, Binding& b, std::string& why) {
    const std::string t = trim(text);
    const std::string l = lower(t);
    if (l == "mouse") {
        b = {Binding::kMouse, 0, {}};
        return true;
    }
    const size_t colon = t.find(':');
    if (colon == std::string::npos) {
        why = "'" + t + "' isn't an input: write pad:<button>, key:<key> or mouse";
        return false;
    }
    const std::string kind = lower(trim(t.substr(0, colon))), name = trim(t.substr(colon + 1));
    if (kind == "pad") {
        for (const PadName& p : kPadNames)
            if (lower(name) == lower(p.name)) {
                b = {Binding::kPad, p.bit, {}};
                return true;
            }
        why = "'" + name + "' isn't a pad button (Up Down Left Right A B X Y Start Back Guide LB RB LT RT L3 R3)";
        return false;
    }
    if (kind == "key") {
        if (name.empty()) {
            why = "key: needs a key's name";
            return false;
        }
        if (key_reserved(name)) {
            why = "key:" + name + " is kept for the program (Escape and F1 open the settings menu, F3 the frame times, F11 is full screen)";
            return false;
        }
        b = {Binding::kKey, 0, name};
        return true;
    }
    why = "'" + kind + "' isn't a kind of input (pad, key, mouse)";
    return false;
}

Binding pad(uint32_t bit) { return {Binding::kPad, bit, {}}; }
Binding key(const char* name) { return {Binding::kKey, 0, name}; }

const char* scaling_name(host::Scaling s) {
    switch (s) {
        case host::Scaling::integer: return "integer";
        case host::Scaling::stretch: return "stretch";
        case host::Scaling::fit: break;
    }
    return "fit";
}

}  // namespace

const char* control_name(Control c) { return kControlNames[c]; }
const char* control_label(Control c) { return kControlLabels[c]; }

const char* pad_button_name(uint32_t bit) {
    for (const PadName& p : kPadNames)
        if (p.bit == bit) return p.name;
    return nullptr;
}

bool key_reserved(const std::string& name) {
    const std::string l = lower(trim(name));
    return l == "escape" || l == "f1" || l == "f3" || l == "f11";
}

std::string binding_text(const Binding& b) {
    switch (b.kind) {
        case Binding::kMouse: return "mouse";
        case Binding::kKey: return "key:" + b.key;
        case Binding::kPad:
            if (const char* n = pad_button_name(b.pad)) return std::string("pad:") + n;
    }
    return "?";
}

std::string binding_label(const Binding& b) {
    switch (b.kind) {
        case Binding::kMouse: return "Mouse";
        case Binding::kKey: return b.key;
        case Binding::kPad:
            for (const PadName& p : kPadNames)
                if (p.bit == b.pad) return p.label;
    }
    return "?";
}

Settings default_settings() {
    Settings s;
    s.controls[kCtlUp] = {pad(host::kPadUp), key("Up")};
    s.controls[kCtlDown] = {pad(host::kPadDown), key("Down")};
    s.controls[kCtlLeft] = {pad(host::kPadLeft), key("Left")};
    s.controls[kCtlRight] = {pad(host::kPadRight), key("Right")};
    s.controls[kCtlOne] = {pad(host::kPadB), key("Z")};
    s.controls[kCtlTwo] = {pad(host::kPadA), key("X")};
    s.controls[kCtlA] = {pad(host::kPadX), pad(host::kPadY), {Binding::kMouse, 0, {}}, key("C"), key("Space")};
    s.controls[kCtlPlus] = {pad(host::kPadStart), key("Return")};
    s.controls[kCtlMinus] = {pad(host::kPadBack), key("Backspace")};
    s.controls[kCtlHome] = {pad(host::kPadGuide), key("H")};
    return s;
}

bool apply_preset(Settings& s, const std::string& name) {
    if (name == "original" || name == "low-end") {  // the console's picture: 1x, its filtering (a Pi 4 does exactly this)
        s.scale = 1, s.anisotropy = 1, s.vsync = true;
        s.true_color = name == "low-end";  // original: the console's 18-bit colour and its dither; low-end: 24 bits (no cost)
    } else if (name == "desktop") {  // a desktop GPU: 3x (1920x1584 frame buffer), 16x anisotropic
        s.scale = 0, s.anisotropy = 16, s.vsync = true;  // scale: auto (the screen's height)
        s.true_color = true;
    } else {
        return false;
    }
    s.preset = name;
    return true;
}

host::Picture picture_of(const Settings& s) {
    host::Picture p;
    p.scaling = s.scaling;
    p.smooth = s.smooth;
    p.look = s.look;
    p.scanlines = float(s.scanlines) / 100.0f;
    p.mask = float(s.mask) / 100.0f;
    p.vsync = s.vsync;
    p.match_refresh = s.match_refresh;
    p.variable_refresh = s.variable_refresh;
    return p;
}

bool parse_settings(const std::string& text, Settings& out, std::string& error, bool legacy) {
    Settings s = default_settings();
    std::istringstream in(text);
    std::string line, section;
    std::set<std::string> seen;
    for (int n = 1; std::getline(in, line); ++n) {
        const auto fail = [&](const std::string& why) {
            error = "line " + std::to_string(n) + ": " + why;
            return false;
        };
        if (n == 1 && line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
        if (const size_t hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']') return fail("a section is written [name]");
            section = lower(trim(line.substr(1, line.size() - 2)));
            if (section != "display" && section != "controls" && section != "console" && !(legacy && section == "video"))
                return fail("there's no section [" + section + "] (display, controls, console)");
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return fail("expected name = value");
        const std::string name = lower(trim(line.substr(0, eq))), value = trim(line.substr(eq + 1));
        if (section.empty()) return fail("'" + name + "' comes before any [section]");
        if (!seen.insert(section + "." + name).second) return fail("'" + name + "' is set twice");
        const std::string v = lower(value);
        bool ok = true;
        const auto boolean = [&](bool& to) {
            if (v == "true") to = true;
            else if (v == "false") to = false;
            else ok = false;
            return ok ? true : fail(name + " is true or false");
        };
        const auto number = [&](unsigned& to, unsigned lo, unsigned hi) {
            unsigned x = 0;
            ok = !v.empty() && v.size() <= 4;
            for (char ch : v) ok = ok && std::isdigit(static_cast<unsigned char>(ch)), x = x * 10 + unsigned(ch - '0');
            if (!ok || x < lo || x > hi) return fail(name + " is a whole number from " + std::to_string(lo) + " to " + std::to_string(hi));
            to = x;
            return true;
        };
        if (section == "video") {  // v0.17.0's
            if (name != "scale") return fail("[video] has no '" + name + "' (scale)");
            if (!number(s.scale, 1, 8)) return false;
            continue;
        }
        if (section == "display") {
            if (name == "preset") {
                if (!apply_preset(s, v)) return fail("the presets are original, low-end and desktop");
            } else if (name == "fullscreen") {
                if (!boolean(s.fullscreen)) return false;
            } else if (name == "monitor") {
                s.monitor = value;
            } else if (name == "scaling") {
                if (v == "integer") s.scaling = host::Scaling::integer;
                else if (v == "fit") s.scaling = host::Scaling::fit;
                else if (v == "stretch") s.scaling = host::Scaling::stretch;
                else return fail("scaling is integer, fit or stretch");
            } else if (name == "smooth") {
                if (!boolean(s.smooth)) return false;
            } else if (name == "look") {
                if (v == "plain") s.look = host::Look::plain;
                else if (v == "crt") s.look = host::Look::crt;
                else return fail("look is plain or crt");
            } else if (name == "scanlines") {
                if (!number(s.scanlines, 0, 100)) return false;
            } else if (name == "mask") {
                if (!number(s.mask, 0, 100)) return false;
            } else if (name == "internal_scale") {
                if (v == "auto" || v == "0") s.scale = 0;  // auto
                else if (!number(s.scale, 1, 8)) return false;
            } else if (name == "anisotropy") {
                if (!number(s.anisotropy, 1, 16)) return false;
                if (s.anisotropy & (s.anisotropy - 1)) return fail("anisotropy is 1, 2, 4, 8 or 16");
            } else if (name == "true_color") {
                if (!boolean(s.true_color)) return false;
            } else if (name == "vsync") {
                if (!boolean(s.vsync)) return false;
            } else if (name == "gpu") {
                s.gpu = value;
            } else if (name == "performance_overlay") {
                if (!boolean(s.performance_overlay)) return false;
            } else if (name == "match_refresh") {
                if (!boolean(s.match_refresh)) return false;
            } else if (name == "variable_refresh") {
                if (!boolean(s.variable_refresh)) return false;
            } else {
                return fail("[display] has no '" + name +
                            "' (preset fullscreen monitor scaling smooth look scanlines mask internal_scale anisotropy true_color vsync gpu "
                            "performance_overlay match_refresh variable_refresh)");
            }
            continue;
        }
        if (section == "console") {
            if (name == "language") {
                if (!number(s.language, 1, 5)) return fail("language is 1 (English), 2 (German), 3 (French), 4 (Spanish) or 5 (Italian)");
            } else if (name == "widescreen") {
                if (!boolean(s.widescreen)) return false;
            } else if (name == "sound") {
                if (v == "mono") s.sound = 0;
                else if (v == "stereo") s.sound = 1;
                else return fail("sound is mono or stereo");
            } else {
                return fail("[console] has no '" + name + "' (language widescreen sound)");
            }
            continue;
        }
        // [controls]
        if (name == "controller1") { s.controller1 = value; continue; }
        if (name == "controller2") { s.controller2 = value; continue; }
        if (name == "stick_threshold") { if (!number(s.stick_threshold, 5, 95)) return false; continue; }
        if (name == "trigger_threshold") { if (!number(s.trigger_threshold, 5, 95)) return false; continue; }
        if (name == "pointer_deadzone") { if (!number(s.pointer_deadzone, 0, 95)) return false; continue; }
        if (name == "rumble_strength") { if (!number(s.rumble_strength, 0, 100)) return false; continue; }
        if (name == "volume") { if (!number(s.volume, 0, 100)) return false; continue; }
        if (name == "mute_unfocused") { if (!boolean(s.mute_unfocused)) return false; continue; }
        if (name == "rumble") {
            if (!boolean(s.rumble)) return false;
            continue;
        }
        if (name == "speaker") {
            if (!boolean(s.speaker)) return false;
            continue;
        }
        if (name == "second_player") {
            if (v == "off") s.second_player = 0;
            else if (v == "second controller") s.second_player = 1;
            else if (v == "first controller") s.second_player = 2;
            else return fail("second_player is off, second controller or first controller");
            continue;
        }
        unsigned c = 0;
        while (c < kControlCount && name != kControlNames[c]) ++c;
        if (c == kControlCount)
            return fail("'" + name + "' isn't a Wii Remote button (up down left right one two a plus minus home), rumble, speaker or second_player");
        s.controls[c].clear();
        std::istringstream items(value);
        for (std::string item; std::getline(items, item, ',');) {
            if (trim(item).empty()) continue;
            if (legacy && lower(trim(item)) == "key:escape") item = "key:H";  // Escape opens the menu now
            Binding b;
            std::string why;
            if (!parse_binding(item, b, why)) return fail(why);
            s.controls[c].push_back(b);
        }
    }
    if (!s.controller1.empty() && s.controller1 == s.controller2) { error = "player one and player two cannot use the same explicit controller"; return false; }
    error.clear();
    out = std::move(s);
    return true;
}

std::string settings_text(const Settings& s) {
    const auto b = [](bool v) { return v ? "true" : "false"; };
    std::string t =
        "# Punch-Out!! QilinRecomp settings. The settings menu (F1 or Esc in the game) changes these and saves\n"
        "# them here; any text editor works too. Delete the file to get the defaults back.\n"
        "# Each line is name = value; # starts a comment.\n"
        "\n"
        "[display]\n"
        "# A preset sets the enhancements below it: original (the console's picture), low-end (a Raspberry Pi\n"
        "# 4-class board), desktop (3x resolution, 16x anisotropic filtering). Keys after it override it.\n"
        "preset = " + s.preset + "\n"
        "# Borderless full screen (F11 or Alt+Enter switch it), and on which display (its name; empty: the main one).\n"
        "fullscreen = " + b(s.fullscreen) + "\n"
        "monitor = " + s.monitor + "\n"
        "# The picture's size: integer (whole multiples of the console's, the sharpest), fit (the largest that\n"
        "# keeps its shape) or stretch (the whole window). smooth: filtered; false: nearest pixel.\n"
        "scaling = " + scaling_name(s.scaling) + "\n"
        "smooth = " + b(s.smooth) + "\n"
        "# Its look: plain, or crt (a CRT television: raster lines and phosphor stripes). For crt, how dark the\n"
        "# gaps between lines are and how strong the stripes, 0 to 100.\n"
        "look = " + (s.look == host::Look::crt ? "crt" : "plain") + "\n"
        "scanlines = " + std::to_string(s.scanlines) + "\n"
        "mask = " + std::to_string(s.mask) + "\n"
        "# The game's resolution, as a multiple of the Wii's 640x528: 1 to 8, or auto (the smallest that fills\n"
        "# the screen line for line). Higher is sharper and needs a faster GPU: the GPU's work grows with the\n"
        "# square (8x is 64 times 1x's pixels). PUNCHOUT_SCALE in the environment overrides it.\n"
        "internal_scale = " + (s.scale ? std::to_string(s.scale) : std::string("auto")) + "\n"
        "# Texture filtering at an angle: 1 (the console's), 2, 4, 8 or 16.\n"
        "anisotropy = " + std::to_string(s.anisotropy) + "\n"
        "# Colour depth: true, 24-bit colour (smooth gradients); false, the console's 18 bits with its dither\n"
        "# pattern, as on a Wii (the game draws fights that way).\n"
        "true_color = " + b(s.true_color) + "\n"
        "# vsync: true, no tearing; false, the lowest latency.\n"
        "vsync = " + b(s.vsync) + "\n"
        "# The graphics device, by name (empty: the best one).\n"
        "gpu = " + s.gpu + "\n"
        "# Frame rate, 1% and 0.1% lows and recent frame times in a corner (F3 switches it).\n"
        "performance_overlay = " + b(s.performance_overlay) + "\n"
        "# Fullscreen on a display whose refresh isn't a multiple of 60 Hz (75, 144, 165 Hz): switch to the\n"
        "# highest refresh at the same resolution that is (120 Hz on a 144 Hz monitor), so frames are even.\n"
        "match_refresh = " + b(s.match_refresh) + "\n"
        "# A G-Sync, FreeSync or VRR display with it on in its driver: no switch and no locking to the\n"
        "# refresh; the display follows the game's 60 frames a second.\n"
        "variable_refresh = " + b(s.variable_refresh) + "\n"
        "\n"
        "[controls]\n"
        "# Preferred device identities selected in the menu; empty: automatic.\n"
        "controller1 = " + s.controller1 + "\n"
        "controller2 = " + s.controller2 + "\n"
        "# Press thresholds, 5-95 percent. Release is 5 points lower to resist jitter.\n"
        "stick_threshold = " + std::to_string(s.stick_threshold) + "\n"
        "trigger_threshold = " + std::to_string(s.trigger_threshold) + "\n"
        "# Pointer dead zone, 0-95 percent.\n"
        "pointer_deadzone = " + std::to_string(s.pointer_deadzone) + "\n"
        "rumble_strength = " + std::to_string(s.rumble_strength) + "\n"
        "# Playback volume and optional muting while another window has focus.\n"
        "volume = " + std::to_string(s.volume) + "\n"
        "mute_unfocused = " + b(s.mute_unfocused) + "\n"
        "# The Wii Remote's rumble, on the controller.\n"
        "rumble = " + b(s.rumble) + "\n"
        "# The sounds the game plays on the Wii Remote's own speaker, from the PC's speakers.\n"
        "speaker = " + b(s.speaker) + "\n"
        "# A second player (Head-to-Head), with a controller as a second Wii Remote, by the same pad buttons:\n"
        "#   off                every controller plays the first Wii Remote\n"
        "#   second controller  the first controller to have a button pressed plays the first Wii Remote (with\n"
        "#                      the keyboard and mouse), the next one the second\n"
        "#   first controller   player one has the keyboard and mouse, the controller is the second Wii Remote\n"
        "second_player = " + std::string(s.second_player == 1 ? "second controller" : s.second_player == 2 ? "first controller" : "off") + "\n"
        "# The Wii Remote held sideways, buttons only. Each of its buttons takes a list of inputs, separated by\n"
        "# commas; an empty list leaves it unused. The settings menu's Controls page sets them by pressing them.\n"
        "#   pad:<button>  a gamepad's button by an Xbox pad's names: Up Down Left Right A B X Y Start Back Guide\n"
        "#                 LB RB LT RT L3 R3 (the left stick counts as Up/Down/Left/Right past half way)\n"
        "#   key:<name>    a key, as SDL names it: A-Z, 0-9, Up, Down, Left, Right, Space, Return, Backspace,\n"
        "#                 Tab, Left Shift, Right Ctrl, F2 ... (not Escape, F1 or F11: they're the program's)\n"
        "#   mouse         the mouse's left button\n"
        "# up/down/left/right are the player's directions (dodge, duck, block). one and two punch left and right;\n"
        "# a is the Star Punch, and the menus' OK. The pointer follows the mouse and the right stick.\n";
    for (unsigned c = 0; c < kControlCount; ++c) {
        t += kControlNames[c];
        t += " =";
        for (size_t i = 0; i < s.controls[c].size(); ++i) t += (i ? ", " : " ") + binding_text(s.controls[c][i]);
        t += "\n";
    }
    t += "\n"
         "[console]\n"
         "# What the game reads as the Wii's own settings, from the next start.\n"
         "# language: 1 English, 2 German, 3 French, 4 Spanish, 5 Italian.\n"
         "language = " + std::to_string(s.language) + "\n"
         "# widescreen: 16:9; false, 4:3.\n"
         "widescreen = " + b(s.widescreen) + "\n"
         "# sound: mono or stereo.\n"
         "sound = " + (s.sound == 0 ? "mono" : "stereo") + "\n";
    return t;
}

std::filesystem::path settings_file(const std::filesystem::path& folder) { return folder / "punchout.ini"; }

bool save_settings(const std::filesystem::path& folder, const Settings& s) {
    const std::filesystem::path file = settings_file(folder), tmp = file.string() + ".tmp";
    // Flush and close before replacement; either failure leaves the old file intact.
#if defined(_WIN32)
    FILE* out = _wfopen(tmp.c_str(), L"wb");
#else
    FILE* out = std::fopen(tmp.c_str(), "wb");
#endif
    if (!out) return false;
    const std::string text = settings_text(s);
    bool ok = std::fwrite(text.data(), 1, text.size(), out) == text.size();
    ok = qilin::rt::sync_file(out) && ok;
    ok = std::fclose(out) == 0 && ok;
    std::error_code ec;
    if (ok) {
        std::filesystem::rename(tmp, file, ec);
        ok = !ec;
    }
    if (!ok) std::filesystem::remove(tmp, ec);
    return ok;
}

Settings load_settings(const std::filesystem::path& folder) {
    const std::filesystem::path file = settings_file(folder), old = folder / "settings.ini";
    const auto read = [](const std::filesystem::path& f, std::string& text) {
        std::ifstream in(f, std::ios::binary);
        if (!in) return false;
        std::stringstream buf;
        buf << in.rdbuf();
        text = buf.str();
        return !in.bad() && !buf.bad();
    };
    std::string text;
    if (read(file, text)) {
        Settings s;
        std::string error;
        if (!parse_settings(text, s, error))
            qilin::rt::fatal(nullptr, "%s, %s (fix it, or delete the file to get the defaults back)", file.string().c_str(),
                             error.c_str());
        qilin::rt::log("settings: read %s", file.string().c_str());
        return s;
    }
    std::error_code read_ec;
    if (std::filesystem::exists(file, read_ec) || read_ec)
        qilin::rt::fatal(nullptr, "cannot read %s (the existing settings have been kept)", file.string().c_str());
    Settings s = default_settings();
    if (read(old, text)) {  // v0.17.0's: its controls and scale carry over
        std::string error;
        if (!parse_settings(text, s, error, true))
            qilin::rt::fatal(nullptr, "%s, %s (fix it, or delete the file to get the defaults back)", old.string().c_str(),
                             error.c_str());
    }
    if (!save_settings(folder, s)) qilin::rt::fatal(nullptr, "cannot write %s", file.string().c_str());
    if (!text.empty()) {
        std::error_code ec;
        std::filesystem::rename(old, folder / "settings.ini.old", ec);
        qilin::rt::log("settings: moved %s into %s (the old file is settings.ini.old)", old.string().c_str(), file.string().c_str());
    } else {
        qilin::rt::log("settings: wrote the defaults to %s", file.string().c_str());
    }
    return s;
}

}  // namespace punchout
