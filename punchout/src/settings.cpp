#include "settings.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

#include "qilin/host/pad.hpp"
#include "qilin/rt/system.hpp"

namespace punchout {
namespace host = qilin::host;

namespace {

constexpr const char* kControlNames[kControlCount] = {"up", "down", "left", "right", "one",
                                                      "two", "a", "plus", "minus", "home"};
struct PadName {
    const char* name;
    uint32_t bit;
};
constexpr PadName kPadNames[] = {
    {"Up", host::kPadUp}, {"Down", host::kPadDown}, {"Left", host::kPadLeft}, {"Right", host::kPadRight},
    {"A", host::kPadA},   {"B", host::kPadB},       {"X", host::kPadX},       {"Y", host::kPadY},
    {"Start", host::kPadStart}, {"Back", host::kPadBack}, {"Guide", host::kPadGuide},
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
        why = "'" + name + "' isn't a pad button (Up Down Left Right A B X Y Start Back Guide)";
        return false;
    }
    if (kind == "key") {
        if (name.empty()) {
            why = "key: needs a key's name";
            return false;
        }
        b = {Binding::kKey, 0, name};
        return true;
    }
    why = "'" + kind + "' isn't a kind of input (pad, key, mouse)";
    return false;
}

std::string binding_text(const Binding& b) {
    switch (b.kind) {
        case Binding::kMouse: return "mouse";
        case Binding::kKey: return "key:" + b.key;
        case Binding::kPad:
            for (const PadName& p : kPadNames)
                if (p.bit == b.pad) return std::string("pad:") + p.name;
    }
    return "?";
}

Binding pad(uint32_t bit) { return {Binding::kPad, bit, {}}; }
Binding key(const char* name) { return {Binding::kKey, 0, name}; }

}  // namespace

const char* control_name(Control c) { return kControlNames[c]; }

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
    s.controls[kCtlHome] = {pad(host::kPadGuide), key("Escape")};
    return s;
}

bool parse_settings(const std::string& text, Settings& out, std::string& error) {
    Settings s = default_settings();
    std::istringstream in(text);
    std::string line, section;
    bool seen[kControlCount] = {};
    for (int n = 1; std::getline(in, line); ++n) {
        const auto fail = [&](const std::string& why) {
            error = "line " + std::to_string(n) + ": " + why;
            return false;
        };
        if (const size_t hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']') return fail("a section is written [name]");
            section = lower(trim(line.substr(1, line.size() - 2)));
            if (section != "video" && section != "controls") return fail("there's no section [" + section + "] (video, controls)");
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return fail("expected name = value");
        const std::string name = lower(trim(line.substr(0, eq))), value = trim(line.substr(eq + 1));
        if (section.empty()) return fail("'" + name + "' comes before any [section]");
        if (section == "video") {
            if (name != "scale") return fail("[video] has no '" + name + "' (scale)");
            if (value.size() != 1 || value[0] < '1' || value[0] > '8') return fail("scale is a whole number from 1 to 8");
            s.scale = static_cast<unsigned>(value[0] - '0');
            continue;
        }
        unsigned c = 0;
        while (c < kControlCount && name != kControlNames[c]) ++c;
        if (c == kControlCount) return fail("'" + name + "' isn't a Wii Remote button (up down left right one two a plus minus home)");
        if (seen[c]) return fail("'" + name + "' is set twice");
        seen[c] = true;
        s.controls[c].clear();
        std::istringstream items(value);
        for (std::string item; std::getline(items, item, ',');) {
            if (trim(item).empty()) continue;
            Binding b;
            std::string why;
            if (!parse_binding(item, b, why)) return fail(why);
            s.controls[c].push_back(b);
        }
    }
    out = std::move(s);
    return true;
}

std::string settings_text(const Settings& s) {
    std::string t =
        "# Punch-Out!! QilinComp settings. Edit with any text editor; delete the file to get the defaults back.\n"
        "# Each line is name = value; # starts a comment.\n"
        "\n"
        "[video]\n"
        "# The picture's resolution, as a multiple of the Wii's 640x528: 1 to 8. Higher is sharper and needs\n"
        "# a faster GPU; 1 suits a Raspberry Pi 4. PUNCHOUT_SCALE in the environment overrides it.\n"
        "scale = " + std::to_string(s.scale) + "\n"
        "\n"
        "[controls]\n"
        "# The Wii Remote held sideways, buttons only. Each of its buttons takes a list of inputs, separated by\n"
        "# commas; an empty list leaves it unused.\n"
        "#   pad:<button>  a gamepad's button by an Xbox pad's names: Up Down Left Right A B X Y Start Back Guide\n"
        "#                 (the left stick counts as Up/Down/Left/Right past half way)\n"
        "#   key:<name>    a key, as SDL names it: A-Z, 0-9, Up, Down, Left, Right, Space, Return, Backspace,\n"
        "#                 Escape, Tab, Left Shift, Right Ctrl, F1 ...\n"
        "#   mouse         the mouse's left button\n"
        "# up/down/left/right are the player's directions (dodge, duck, block). one and two punch left and right;\n"
        "# a is the Star Punch, and the menus' OK. The pointer follows the mouse and the right stick.\n";
    for (unsigned c = 0; c < kControlCount; ++c) {
        t += kControlNames[c];
        t += " =";
        for (size_t i = 0; i < s.controls[c].size(); ++i) t += (i ? ", " : " ") + binding_text(s.controls[c][i]);
        t += "\n";
    }
    return t;
}

Settings load_settings(const std::filesystem::path& folder) {
    const std::filesystem::path file = folder / "settings.ini";
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        Settings s = default_settings();
        std::ofstream outf(file, std::ios::binary);
        outf << settings_text(s);
        if (!outf) qilin::rt::fatal(nullptr, "cannot write %s", file.string().c_str());
        qilin::rt::log("settings: wrote the defaults to %s", file.string().c_str());
        return s;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    Settings s;
    std::string error;
    if (!parse_settings(buf.str(), s, error))
        qilin::rt::fatal(nullptr, "%s, %s (fix it, or delete the file to get the defaults back)", file.string().c_str(),
                         error.c_str());
    qilin::rt::log("settings: read %s", file.string().c_str());
    return s;
}

}  // namespace punchout
