// The settings file (punchout/src/settings.cpp): what it writes reads back
// the same, edits are read, and mistakes are reported with their line.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "settings.hpp"
#include "qilin/host/pad.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

bool same(const punchout::Settings& a, const punchout::Settings& b) {
    if (a.scale != b.scale || a.preset != b.preset || a.fullscreen != b.fullscreen || a.monitor != b.monitor ||
        a.scaling != b.scaling || a.smooth != b.smooth || a.look != b.look || a.scanlines != b.scanlines || a.mask != b.mask ||
        a.anisotropy != b.anisotropy || a.true_color != b.true_color || a.vsync != b.vsync || a.gpu != b.gpu || a.rumble != b.rumble || a.second_player != b.second_player || a.speaker != b.speaker ||
        a.controller1 != b.controller1 || a.controller2 != b.controller2 || a.stick_threshold != b.stick_threshold ||
        a.trigger_threshold != b.trigger_threshold || a.pointer_deadzone != b.pointer_deadzone || a.rumble_strength != b.rumble_strength ||
        a.volume != b.volume || a.mute_unfocused != b.mute_unfocused || a.language != b.language || a.widescreen != b.widescreen || a.sound != b.sound)
        return false;
    for (unsigned c = 0; c < punchout::kControlCount; ++c) {
        if (a.controls[c].size() != b.controls[c].size()) return false;
        for (size_t i = 0; i < a.controls[c].size(); ++i) {
            const auto &x = a.controls[c][i], &y = b.controls[c][i];
            if (x.kind != y.kind || x.pad != y.pad || x.key != y.key) return false;
        }
    }
    return true;
}

// The error a text gives, or "" if it reads.
std::string error_of(const std::string& text, bool legacy = false) {
    punchout::Settings s;
    std::string e;
    return punchout::parse_settings(text, s, e, legacy) ? "" : e;
}

}  // namespace

int main() {
    using punchout::Binding;
    namespace host = qilin::host;
    const punchout::Settings d = punchout::default_settings();

    {
        punchout::Settings s;
        std::string e;
        check(punchout::parse_settings(punchout::settings_text(d), s, e) && same(s, d), "the defaults read back as written");
    }
    {
        punchout::Settings s;
        std::string e;
        check(punchout::parse_settings("", s, e) && same(s, d), "an empty file means the defaults");
    }
    {
        punchout::Settings s;
        std::string e;
        const bool ok = punchout::parse_settings(
            "[Display]\n"
            "internal_scale = 3   # sharper\n"
            "\n"
            "[controls]\n"
            "  TWO = pad:Y , KEY:Right Shift,mouse\n"
            "home =\n",
            s, e);
        check(ok && s.scale == 3, "internal_scale is read, sections and names in any case");
        check(ok && s.controls[punchout::kCtlTwo].size() == 3 && s.controls[punchout::kCtlTwo][0].pad == host::kPadY &&
                  s.controls[punchout::kCtlTwo][1].kind == Binding::kKey &&
                  s.controls[punchout::kCtlTwo][1].key == "Right Shift" &&
                  s.controls[punchout::kCtlTwo][2].kind == Binding::kMouse,
              "a control takes pads, keys with spaces in their names, and the mouse");
        check(ok && s.controls[punchout::kCtlHome].empty(), "an empty list leaves a control unused");
        punchout::Settings untouched = s;
        untouched.scale = d.scale;
        untouched.controls[punchout::kCtlTwo] = d.controls[punchout::kCtlTwo];
        untouched.controls[punchout::kCtlHome] = d.controls[punchout::kCtlHome];
        check(ok && same(untouched, d), "controls not mentioned keep their defaults");
    }
    check(error_of("[controls]\njab = pad:A\n").rfind("line 2: 'jab' isn't a Wii Remote button", 0) == 0,
          "an unknown control is reported with its line");
    check(error_of("[controls]\none = pad:Z\n").rfind("line 2: 'Z' isn't a pad button", 0) == 0, "an unknown pad button");
    check(error_of("[controls]\none = Z\n").rfind("line 2: 'Z' isn't an input", 0) == 0, "an input without its kind");
    check(error_of("[display]\ninternal_scale = 9\n").rfind("line 2: internal_scale is a whole number from 1 to 8", 0) == 0,
          "a scale out of range");
    {
        punchout::Settings s;
        std::string e;
        check(punchout::parse_settings("[display]\ninternal_scale = auto\nmatch_refresh = false\nvariable_refresh = true\n", s, e) &&
                  s.scale == 0 && !s.match_refresh && s.variable_refresh,
              "auto scale and the refresh switches");
    }
    check(error_of("[video]\nscale = 2\n").rfind("line 1: there's no section [video]", 0) == 0, "v0.17.0's [video] only in the old file");
    {
        punchout::Settings s;
        std::string e;
        const bool ok = punchout::parse_settings("[video]\nscale = 4\n[controls]\nhome = pad:Guide, key:Escape\n", s, e, true);
        check(ok && s.scale == 4, "the old file's scale carries over");
        check(ok && s.controls[punchout::kCtlHome].size() == 2 && s.controls[punchout::kCtlHome][1].key == "H",
              "the old file's Escape (now the menu's) becomes H");
    }
    check(error_of("[controls]\nplus = key:F1\n").find("kept for the program") != std::string::npos, "F1 can't be bound");
    check(error_of("[controls]\nplus = key:escape\n").find("kept for the program") != std::string::npos, "nor Escape");
    {
        punchout::Settings s;
        std::string e;
        const bool ok = punchout::parse_settings("[display]\npreset = desktop\nanisotropy = 4\n", s, e);
        check(ok && s.scale == 0 && s.anisotropy == 4 && s.preset == "desktop", "a preset, then a key that overrides it");
        check(!punchout::parse_settings("[display]\npreset = huge\n", s, e) && e.find("presets are") != std::string::npos,
              "an unknown preset");
        check(error_of("[display]\nanisotropy = 3\n").find("1, 2, 4, 8 or 16") != std::string::npos, "anisotropy is a power of two");
        punchout::Settings o, l, t;
        std::string eo;
        check(punchout::parse_settings("[display]\npreset = original\n", o, eo) && !o.true_color &&
                  punchout::parse_settings("[display]\npreset = low-end\n", l, eo) && l.true_color &&
                  punchout::parse_settings("[display]\npreset = original\ntrue_color = true\n", t, eo) && t.true_color && s.true_color,
              "true colour: off in the original preset alone, and a key of its own");
    }
    {
        punchout::Settings s = d;
        s.preset = "desktop", s.fullscreen = true, s.monitor = "DELL U2720Q", s.scaling = qilin::host::Scaling::integer;
        s.smooth = false, s.look = qilin::host::Look::crt, s.scanlines = 80, s.mask = 0, s.scale = 5, s.anisotropy = 8;
        s.vsync = false, s.gpu = "AMD Radeon RX 6600", s.rumble = false, s.second_player = 1, s.speaker = false, s.language = 3, s.widescreen = false, s.sound = 0;
        s.controller1 = "guid:n:70616431:0", s.controller2 = "guid:n:70616432:0";
        s.stick_threshold = 20, s.trigger_threshold = 30, s.pointer_deadzone = 10, s.rumble_strength = 45;
        s.volume = 65, s.mute_unfocused = true;
        s.controls[punchout::kCtlOne] = {{Binding::kPad, host::kPadLT, {}}, {Binding::kKey, 0, "Left Shift"}};
        punchout::Settings back;
        std::string e;
        check(punchout::parse_settings(punchout::settings_text(s), back, e) && same(back, s), "every setting reads back as written");
    }
    check(error_of("[display]\nlook = vhs\n").find("look is plain or crt") != std::string::npos, "an unknown look");
    check(error_of("[display]\nscanlines = 101\n").find("from 0 to 100") != std::string::npos, "scanlines out of range");
    check(error_of("[console]\nlanguage = 0\n").find("language is 1 (English)") != std::string::npos, "a language not on the disc");
    check(error_of("[controls]\nrumble = maybe\n").find("rumble is true or false") != std::string::npos, "a boolean that isn't");
    check(error_of("[controls]\nsecond_player = third controller\n").find("second_player is off") != std::string::npos, "a second player that isn't");
    {
        punchout::Settings t;
        std::string e;
        check(punchout::parse_settings("[controls]\nsecond_player = first controller\n", t, e) && t.second_player == 2 && d.second_player == 0,
              "a second player on the first controller; none by default");
    }
    check(error_of("scale = 2\n").rfind("line 1: 'scale' comes before any [section]", 0) == 0, "a setting outside a section");
    check(error_of("[audio]\n").rfind("line 1: there's no section [audio]", 0) == 0, "an unknown section");
    {
        // The data folder's file: written with the defaults, from v0.17.0's when that's there.
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "punchout_settings_test";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        {
            std::ofstream old(dir / "settings.ini");
            old << "[video]\nscale = 2\n[controls]\nhome = key:Escape\n";
        }
        const punchout::Settings s = punchout::load_settings(dir);
        check(s.scale == 2 && std::filesystem::exists(dir / "punchout.ini") && std::filesystem::exists(dir / "settings.ini.old") &&
                  !std::filesystem::exists(dir / "settings.ini"),
              "v0.17.0's settings.ini becomes punchout.ini");
        punchout::Settings changed = s;
        changed.look = qilin::host::Look::crt;
        check(punchout::save_settings(dir, changed) && punchout::load_settings(dir).look == qilin::host::Look::crt,
              "saving, then loading, keeps a change");
        std::filesystem::remove_all(dir);
    }
    check(error_of("[controls]\na = pad:A\na = pad:B\n").rfind("line 3: 'a' is set twice", 0) == 0, "a control set twice");
    check(error_of("[controls]\r\na = pad:A\r\n").empty(), "Windows line endings");

    check(error_of("[controls]\ncontroller1 = same\ncontroller2 = same\n").find("same explicit controller") != std::string::npos,
          "one device cannot be assigned to both players");
    check(error_of("[controls]\nstick_threshold = 0\n").find("from 5 to 95") != std::string::npos, "stick threshold range");
    check(error_of("[controls]\nvolume = 101\n").find("from 0 to 100") != std::string::npos, "volume range");
    check(error_of("[display]\nvsync = true\nVSYNC = false\n").find("set twice") != std::string::npos, "duplicate scalar settings rejected");
    check(error_of("\xEF\xBB\xBF[display]\ninternal_scale = AUTO\n").empty(), "BOM and case-insensitive auto");
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
