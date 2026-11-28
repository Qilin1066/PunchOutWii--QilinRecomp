// qilin: Punch-Out!! (Wii) static recompiler (work in progress).
//
//   qilin info    <main.dol> [--json out.json]   SHA-256, supported build, entry point, sections, BSS
//   qilin insns   <main.dol> [-o out.txt]        every word of the text sections, decoded
//                                                (for the binutils cross-check)
//   qilin disasm  <words.bin> [--base addr] [-o out.txt]
//                                                a file of big-endian words, decoded (encoding sweep)
//   qilin profile <main.dol>                     instruction profile of the text sections
//   qilin cfg     <main.dol> [--json out.json]   function discovery + control flow, summary
//   qilin coverage <main.dol> [--list]           word accounting of the text: code / padding / unknown
//   qilin suspects <main.dol>                    functions with problems, for review
//   qilin lift    <main.dol> -o <dir> [--plan plan.tsv] [--files n]
//                                                the game's code as C++ (--out is -o)
//   qilin identify <extracted disc folder> --json [--build <title>]
//                                                what the folder is, for the downloads' launcher: one
//                                                JSON object on stdout (identify.hpp); exit 0 the
//                                                supported build, 3 another, 2 unreadable
//
// Listings are one line per word: address, word, text ("8000310c 4e800020 bclr 20,lt,0").
// Exit status is non-zero on any error; nothing fails silently.
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cfg.hpp"
#include "decoder.hpp"
#include "dol.hpp"
#include "identify.hpp"
#include "lift.hpp"
#include "qilin/command_line.hpp"
#include "qilin/path.hpp"
#include "qilin/rt/file.hpp"

#ifndef QILIN_VERSION
#define QILIN_VERSION "unknown"
#endif

using namespace qilin;

namespace {

struct Failure {
    std::string message;
};

int usage() {
    std::print(stderr,
               "qilin " QILIN_VERSION "\n"
               "usage:\n"
               "  qilin info    <main.dol> [--json out.json]\n"
               "  qilin insns   <main.dol> [-o out.txt]\n"
               "  qilin disasm  <words.bin> [--base addr] [-o out.txt]\n"
               "  qilin profile <main.dol>\n"
               "  qilin cfg     <main.dol> [--json out.json]\n"
               "  qilin coverage <main.dol> [--list]\n"
               "  qilin suspects <main.dol>\n"
               "  qilin lift    <main.dol> -o <dir> [--plan plan.tsv] [--files n]\n"
               "  qilin identify <extracted disc folder> --json [--build <title>]\n"
               "  qilin --version\n");
    return 2;
}

struct Args {
    std::string command, input;
    std::optional<std::string> out, json, plan;
    uint32_t base = 0, files = 0;
    bool list = false;
};

uint32_t parse_u32(std::string_view s) {
    int radix = 10;
    if (s.starts_with("0x") || s.starts_with("0X")) {
        s.remove_prefix(2);
        radix = 16;
    }
    uint32_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v, radix);
    if (ec != std::errc{} || end != s.data() + s.size()) throw Failure{std::format("not a number: {}", s)};
    return v;
}

Args parse_args(std::span<char*> argv) {
    Args a;
    a.command = argv[1];
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string_view arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argv.size()) throw Failure{std::format("{} needs a value", arg)};
            return argv[++i];
        };
        if (arg == "-o" || arg == "--out") a.out = value();
        else if (arg == "--json") a.json = value();
        else if (arg == "--base") a.base = parse_u32(value());
        else if (arg == "--plan") a.plan = value();
        else if (arg == "--files") a.files = parse_u32(value());
        else if (arg == "--list") a.list = true;
        else if (arg.starts_with("-")) throw Failure{std::format("unknown option {}", arg)};
        else if (a.input.empty()) a.input = arg;
        else throw Failure{std::format("unexpected argument {}", arg)};
    }
    if (a.input.empty()) throw Failure{"missing input file"};
    return a;
}

Dol load_dol(const std::string& path) {
    auto dol = Dol::load(native_path(path));
    if (!dol) throw Failure{std::format("{}: {}", path, dol.error())};
    return std::move(*dol);
}

// stdout, or the -o file.
class Output {
public:
    explicit Output(const std::optional<std::string>& path) {
        if (!path) return;
        file_ = rt::open_file(native_path(*path), "w");
        if (!file_) throw Failure{std::format("cannot write {}", *path)};
    }
    ~Output() {
        if (file_) std::fclose(file_);
    }
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    std::FILE* get() const { return file_ ? file_ : stdout; }

private:
    std::FILE* file_ = nullptr;
};

void list_words(std::FILE* out, std::span<const uint8_t> bytes, uint32_t base) {
    for (size_t i = 0; i + 4 <= bytes.size(); i += 4) {
        const uint32_t pc = base + static_cast<uint32_t>(i);
        const ppc::Insn in = ppc::decode(read_be32(bytes.data() + i));
        std::println(out, "{:08x} {:08x} {}", pc, in.word, ppc::disasm(in, pc));
    }
}

int cmd_info(const Args& a) {
    const Dol dol = load_dol(a.input);
    const std::string json = to_json(dol);
    if (a.json) {
        std::ofstream f(native_path(*a.json), std::ios::binary);
        if (!(f << json)) throw Failure{std::format("cannot write {}", *a.json)};
    } else {
        std::print("{}", json);
    }
    return 0;
}

int cmd_insns(const Args& a) {
    const Dol dol = load_dol(a.input);
    Output out(a.out);
    for (const DolSection& s : dol.sections)
        if (s.text) list_words(out.get(), dol.image().subspan(s.address - dol.base(), s.size), s.address);
    return 0;
}

int cmd_disasm(const Args& a) {
    std::ifstream f(native_path(a.input), std::ios::binary);
    if (!f) throw Failure{std::format("cannot open {}", a.input)};
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    if (bytes.size() % 4) throw Failure{std::format("{}: size is not a multiple of 4", a.input)};
    Output out(a.out);
    list_words(out.get(), bytes, a.base);
    return 0;
}

int cmd_profile(const Args& a) {
    const Dol dol = load_dol(a.input);
    std::map<ppc::Op, uint64_t> counts;
    uint64_t words = 0;
    for (const DolSection& s : dol.sections) {
        if (!s.text) continue;
        for (uint32_t pc = s.address; pc < s.end(); pc += 4, ++words) ++counts[ppc::decode(dol.u32(pc)).op];
    }
    std::vector<std::pair<ppc::Op, uint64_t>> sorted(counts.begin(), counts.end());
    std::ranges::sort(sorted, [](const auto& x, const auto& y) { return x.second > y.second; });
    std::println("{} words in text sections, {} distinct instructions", words,
                 sorted.size() - (counts.contains(ppc::Op::invalid) ? 1 : 0));
    uint64_t paired = 0, privileged = 0, floating = 0;
    for (const auto& [op, n] : sorted) {
        const ppc::Spec& s = ppc::spec(op);
        if (s.flags & ppc::kPaired) paired += n;
        if (s.flags & ppc::kPrivileged) privileged += n;
        if (s.flags & ppc::kFloat) floating += n;
        std::println("{:>10} {:6.3f}%  {}", n, 100.0 * double(n) / double(words),
                     op == ppc::Op::invalid ? std::string_view("(not an instruction)") : s.name);
    }
    std::println("floating point {}, paired single {}, privileged {}", floating, paired, privileged);
    return 0;
}

int cmd_cfg(const Args& a) {
    const Dol dol = load_dol(a.input);
    Cfg cfg(dol);
    cfg.run();
    std::print("{}", cfg.summary());
    if (a.json) {
        std::ofstream f(native_path(*a.json), std::ios::binary);
        if (!(f << cfg.to_json())) throw Failure{std::format("cannot write {}", *a.json)};
    }
    return 0;
}

int cmd_coverage(const Args& a) {
    const Dol dol = load_dol(a.input);
    Cfg cfg(dol);
    cfg.run();
    uint64_t unknown = 0;
    for (const Coverage& c : cfg.coverage()) {
        const uint64_t words = c.code + c.padding + c.dead + c.unknown;
        std::println("{} {:08X}-{:08X}: {} words: code {} ({}), padding {}, dead {}, unknown {} ({}) in {} spans", c.section,
                     c.lo, c.hi, words, c.code, percent(c.code, words, 3), c.padding, c.dead, c.unknown,
                     percent(c.unknown, words, 3), c.unknown_spans.size());
        if (a.list) {
            for (const auto& [lo, hi] : c.unknown_spans) std::println("  {:08X}-{:08X} ({} words)", lo, hi, (hi - lo) / 4);
            for (const auto& [lo, hi] : c.dead_spans) std::println("  dead {:08X}-{:08X} ({} words)", lo, hi, (hi - lo) / 4);
        }
        unknown += c.unknown;
    }
    return unknown ? 3 : 0;
}

int cmd_suspects(const Args& a) {
    const Dol dol = load_dol(a.input);
    Cfg cfg(dol);
    cfg.run();
    for (const Suspect& s : cfg.suspects()) std::println("{:08X} {}", s.start, s.why);
    std::string rejected;
    for (uint32_t r : cfg.rejected()) rejected += std::format(" {:08X}", r);
    std::println("{} suspects; rejected starts: {}{}", cfg.suspects().size(), cfg.rejected().size(), rejected);
    return 0;
}

// Every function, lifted to C++ if it's all in the current slice, else a stub
// that traps with the reason. Writes functions.hpp, lifted_NNN.cpp, table.cpp
// and unlifted.txt into the -o directory.
// The library plan (data/<build>/plan.tsv): address ranges, each lifted,
// replaced (native code at the library's API) or dropped (never runs).
struct PlanRange {
    uint32_t start, end;
    std::string plan, library;
};
std::vector<PlanRange> load_plan(const std::string& path) {
    std::ifstream f(native_path(path));
    if (!f) throw Failure{std::format("cannot read {}", path)};
    std::vector<PlanRange> out;
    std::string line;
    for (unsigned n = 1; std::getline(f, line); ++n) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> col;
        for (size_t i = 0;;) {
            const size_t t = line.find('\t', i);
            col.push_back(line.substr(i, t == std::string::npos ? t : t - i));
            if (t == std::string::npos) break;
            i = t + 1;
        }
        if (col.size() != 4 || (col[2] != "lift" && col[2] != "replace" && col[2] != "drop"))
            throw Failure{std::format("{}:{}: expected start, end, lift|replace|drop, library", path, n)};
        PlanRange r{parse_u32("0x" + col[0]), parse_u32("0x" + col[1]), col[2], col[3]};
        if (r.end <= r.start) throw Failure{std::format("{}:{}: empty range", path, n)};
        for (const PlanRange& o : out)  // disjoint, or one inside the other
            if (r.start < o.end && o.start < r.end && !(r.start >= o.start && r.end <= o.end) && !(o.start >= r.start && o.end <= r.end))
                throw Failure{std::format("{}:{}: {:08X}-{:08X} overlaps {:08X}-{:08X} without nesting", path, n, r.start, r.end, o.start, o.end)};
        out.push_back(std::move(r));
    }
    return out;
}

// A C++ string literal holding s.
std::string cpp_string(std::string_view s) {
    std::string q = "\"";
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') q += '\\', q += static_cast<char>(ch);
        else if (ch < 0x20 || ch >= 0x7F) q += std::format("\\{:03o}", ch);
        else q += static_cast<char>(ch);
    }
    return q + "\"";
}

int cmd_lift(const Args& a) {
    if (!a.out) throw Failure{"lift needs -o <directory>"};
    const Dol dol = load_dol(a.input);
    Cfg cfg(dol);
    cfg.run();
    const std::vector<PlanRange> plan = a.plan ? load_plan(*a.plan) : std::vector<PlanRange>{};
    auto plan_of = [&](uint32_t start) -> const PlanRange* {
        if (!a.plan) return nullptr;
        const PlanRange* best = nullptr;  // the narrowest range holding the function decides
        for (const PlanRange& r : plan)
            if (start >= r.start && start < r.end && (!best || r.end - r.start < best->end - best->start)) best = &r;
        if (!best) throw Failure{std::format("function {:08X} is in no range of the plan", start)};
        return best;
    };
    const std::filesystem::path dir = native_path(*a.out);
    std::filesystem::create_directories(dir);
    // A file whose text is unchanged is left alone, so a build after an update
    // recompiles only what the update changed.
    auto write = [&](const std::string& name, const std::string& text) {
        {
            std::ifstream old(dir / name, std::ios::binary);
            if (old && std::string(std::istreambuf_iterator<char>(old), {}) == text) return;
        }
        std::ofstream f(dir / name, std::ios::binary);
        if (!(f << text)) throw Failure{std::format("cannot write {}", utf8_path(dir / name))};
    };

    const size_t total = cfg.functions().size();
    const size_t kChunk = a.files ? (total + a.files - 1) / a.files : 400;
    std::string header =
        "// Generated by qilin lift from the game's main.dol: do not edit, do not commit.\n"
        "#pragma once\n#include \"qilin/rt/system.hpp\"\n\nnamespace qilin::gen {\n";
    std::string table =
        "// Generated by qilin lift: every function, by address.\n#include \"functions.hpp\"\n\n"
        "namespace qilin::gen {\nextern const FunctionEntry kFunctions[] = {\n";
    header += "using ::qilin::rt::FunctionEntry;\nusing ::qilin::rt::ReplacedEntry;\n"
              "extern const FunctionEntry kFunctions[];\nextern const unsigned kFunctionCount;\n"
              "// Functions the plan replaces or drops: each is a thunk into the runtime's registry.\n"
              "extern const ReplacedEntry kReplaced[];\nextern const unsigned kReplacedCount;\n\n";
    std::string replaced_table =
        "// Generated by qilin lift: the functions the plan replaces or drops.\n#include \"functions.hpp\"\n\n"
        "namespace qilin::gen {\nextern const ReplacedEntry kReplaced[] = {\n";
    std::string chunk, unlifted;
    std::map<std::string, size_t> reasons;
    size_t lifted = 0, stubs = 0, lifted_insns = 0, n = 0, files = 0, thunks = 0, dropped = 0, thunk_insns = 0;
    auto flush = [&] {
        if (chunk.empty() && !a.files) return;
        write(std::format("lifted_{:03}.cpp", files++),
              "// Generated by qilin lift: do not edit, do not commit.\n#include \"functions.hpp\"\n\n"
              "namespace qilin::gen {\nusing namespace ::qilin::rt;\n\n" + chunk + "}  // namespace qilin::gen\n");
        chunk.clear();
    };
    for (const auto& [start, f] : cfg.functions()) {
        header += std::format("void {}(::qilin::rt::Cpu& c);\n", fn_name(start));
        table += std::format("    {{0x{:08X}u, {}}},\n", start, fn_name(start));
        if (const PlanRange* r = plan_of(start); r && r->plan != "lift") {
            chunk += std::format("void {}(Cpu& c) {{ c.hooks->replaced(c, 0x{:08X}u); }}\n\n", fn_name(start), start);
            replaced_table += std::format("    {{0x{:08X}u, {}, {}}},\n", start, cpp_string(r->library), r->plan == "drop");
            ++thunks;
            dropped += r->plan == "drop";
            thunk_insns += f.insns.size();
            if (++n % kChunk == 0) flush();
            continue;
        }
        const Lifted l = lift(lift_input(cfg, f));
        if (l.ok) {
            chunk += l.code + "\n";
            ++lifted;
            lifted_insns += f.insns.size();
        } else {
            chunk += std::format("void {}(Cpu& c) {{ c.hooks->trap(c, 0x{:08X}u, \"not lifted: {} at {:08X}\"); }}\n\n",
                                 fn_name(start), start, l.why, l.why_at);
            unlifted += std::format("{:08X} {} at {:08X}\n", start, l.why, l.why_at);
            ++reasons[l.why];
            ++stubs;
        }
        if (++n % kChunk == 0) flush();
    }
    flush();
    while (a.files && files < a.files) flush();
    if (a.files && files != a.files) throw Failure{std::format("{} functions don't fit {} files", n, a.files)};
    header += "}  // namespace qilin::gen\n";
    replaced_table += std::format("{}}};\nextern const unsigned kReplacedCount = {};\n}}  // namespace qilin::gen\n",
                                  thunks ? "" : "    {0u, nullptr, false},\n", thunks);
    write("replaced.cpp", replaced_table);
    table += std::format("}};\nextern const unsigned kFunctionCount = {};\n}}  // namespace qilin::gen\n", n);
    write("functions.hpp", header);
    write("table.cpp", table);
    write("unlifted.txt", unlifted);

    size_t insns = 0;
    for (const auto& [start, f] : cfg.functions()) insns += f.insns.size();
    if (a.plan) {
        std::println("{} functions: {} lifted, {} replaced and {} dropped (thunks into the runtime), {} stubs; "
                     "{} of the {} instructions outside replaced and dropped code lifted ({}); {} files",
                     n, lifted, thunks - dropped, dropped, stubs, lifted_insns, insns - thunk_insns,
                     percent(lifted_insns, insns - thunk_insns, 1), files + 3);
    } else {
        std::println("{} functions: {} lifted ({}), {} stubs; {} of {} instructions lifted ({}); {} files",
                     n, lifted, percent(lifted, n, 1), stubs, lifted_insns, insns, percent(lifted_insns, insns, 1), files + 3);
    }
    std::vector<std::pair<size_t, std::string>> by;
    for (const auto& [why, k] : reasons) by.emplace_back(k, why);
    std::ranges::sort(by, std::greater{});
    for (size_t k = 0; k < by.size() && k < 15; ++k) std::println("  {:6} {}", by[k].first, by[k].second);
    return 0;
}

// The launcher's contract (identify.hpp): JSON whatever happens, and only
// exit codes 0, 3 and 2.
int cmd_identify(int argc, char** argv) {
    std::optional<std::string> path, build;
    bool json = false;
    std::string problem;
    for (int i = 2; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--json") json = true;
        else if (a == "--build" && i + 1 < argc) build = argv[++i];
        else if (!a.starts_with("-") && !path) path = std::string(a);
        else problem = std::format("identify: unexpected argument {}", a);
    }
    if (!path) problem = "identify takes one folder";
    if (!json) problem = "identify needs --json";
    Identity id;
    if (!problem.empty()) {
        id.message = problem;
    } else {
        try {
            id = identify(native_path(*path), build.value_or(""));
        } catch (const std::exception& e) {
            id = Identity{};
            id.message = std::format("The recompiler failed: {}.", e.what());
        }
    }
    std::print("{}", to_json(id));
    std::fflush(stdout);
    return id.code;
}

}  // namespace

int main(int argc, char** argv) {
    // Windows: the arguments in UTF-8 from the UTF-16 command line, not the
    // ANSI code page (a folder named in Japanese, or one with accents on a Japanese system).
    const qilin::CommandLine command_line(argc, argv);
    argc = command_line.count();
    argv = command_line.values();
    if (argc >= 2 && std::string_view(argv[1]) == "identify") return cmd_identify(argc, argv);
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::println("qilin " QILIN_VERSION);
        return 0;
    }
    if (argc < 3) return usage();
    try {
        const Args a = parse_args(std::span(argv, static_cast<size_t>(argc)));
        if (a.command == "info") return cmd_info(a);
        if (a.command == "insns") return cmd_insns(a);
        if (a.command == "disasm") return cmd_disasm(a);
        if (a.command == "profile") return cmd_profile(a);
        if (a.command == "cfg") return cmd_cfg(a);
        if (a.command == "coverage") return cmd_coverage(a);
        if (a.command == "suspects") return cmd_suspects(a);
        if (a.command == "lift") return cmd_lift(a);
        return usage();
    } catch (const Failure& f) {
        std::println(stderr, "qilin: {}", f.message);
        return 1;
    } catch (const std::exception& e) {
        std::println(stderr, "qilin: internal error: {}", e.what());
        return 1;
    }
}
