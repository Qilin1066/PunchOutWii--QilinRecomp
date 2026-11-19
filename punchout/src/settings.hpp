// The settings file: settings.ini in the per-user folder (host/paths.hpp),
// written with the defaults the first time the game runs. The player edits it
// by hand. A line it can't read stops the program, naming the line, rather
// than being ignored.
//
// Runtime code: no std::format (macOS 10.15).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace punchout {

// The Wii Remote's buttons, held sideways, as the player sees them: up is
// the player's up (the remote's cross pad turned a quarter, wpad.cpp).
enum Control : unsigned {
    kCtlUp, kCtlDown, kCtlLeft, kCtlRight, kCtlOne, kCtlTwo, kCtlA, kCtlPlus, kCtlMinus, kCtlHome, kControlCount
};
const char* control_name(Control c);

// One input that presses a control.
struct Binding {
    enum Kind { kPad, kKey, kMouse } kind = kPad;
    uint32_t pad = 0;  // kPad: a qilin::host::PadButton bit
    std::string key;   // kKey: the key's name, as SDL names keys ("X", "Space", "Left")
};

struct Settings {
    unsigned scale = 1;  // the EFB's resolution, 1 to 8 times the Wii's
    std::vector<Binding> controls[kControlCount];
};

Settings default_settings();

// Read settings from text. On a problem: false, with `error` saying what
// and where ("line 12: ...").
bool parse_settings(const std::string& text, Settings& out, std::string& error);

// The file's text for the given settings, with comments explaining each part.
std::string settings_text(const Settings& s);

// settings.ini in `folder`: read it, or write the defaults there if it's
// missing. A file that doesn't read stops the program.
Settings load_settings(const std::filesystem::path& folder);

}  // namespace punchout
