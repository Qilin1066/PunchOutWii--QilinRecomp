// The settings file (punchout/src/settings.cpp): what it writes reads back
// the same, edits are read, and mistakes are reported with their line.
#include <cstdio>
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
    if (a.scale != b.scale) return false;
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
std::string error_of(const std::string& text) {
    punchout::Settings s;
    std::string e;
    return punchout::parse_settings(text, s, e) ? "" : e;
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
            "[Video]\n"
            "scale = 3   # sharper\n"
            "\n"
            "[controls]\n"
            "  TWO = pad:Y , KEY:Right Shift,mouse\n"
            "home =\n",
            s, e);
        check(ok && s.scale == 3, "scale is read, sections and names in any case");
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
    check(error_of("[video]\nscale = 9\n").rfind("line 2: scale is a whole number from 1 to 8", 0) == 0, "a scale out of range");
    check(error_of("scale = 2\n").rfind("line 1: 'scale' comes before any [section]", 0) == 0, "a setting outside a section");
    check(error_of("[audio]\n").rfind("line 1: there's no section [audio]", 0) == 0, "an unknown section");
    check(error_of("[controls]\na = pad:A\na = pad:B\n").rfind("line 3: 'a' is set twice", 0) == 0, "a control set twice");
    check(error_of("[controls]\r\na = pad:A\r\n").empty(), "Windows line endings");

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
