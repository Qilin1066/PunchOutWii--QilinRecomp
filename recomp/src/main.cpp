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
               "  qilin --version\n");
    return 2;
}

struct Args {
    std::string command, input;
    std::optional<std::string> out, json;
    uint32_t base = 0;
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
        if (arg == "-o") a.out = value();
        else if (arg == "--json") a.json = value();
        else if (arg == "--base") a.base = parse_u32(value());
        else if (arg == "--list") a.list = true;
        else if (arg.starts_with("-")) throw Failure{std::format("unknown option {}", arg)};
        else if (a.input.empty()) a.input = arg;
        else throw Failure{std::format("unexpected argument {}", arg)};
    }
    if (a.input.empty()) throw Failure{"missing input file"};
    return a;
}

Dol load_dol(const std::string& path) {
    auto dol = Dol::load(path);
    if (!dol) throw Failure{std::format("{}: {}", path, dol.error())};
    return std::move(*dol);
}

// stdout, or the -o file.
class Output {
public:
    explicit Output(const std::optional<std::string>& path) {
        if (!path) return;
        file_ = std::fopen(path->c_str(), "w");
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
        std::ofstream f(*a.json, std::ios::binary);
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
    std::ifstream f(a.input, std::ios::binary);
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
        std::ofstream f(*a.json, std::ios::binary);
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
        if (a.list)
            for (const auto& [lo, hi] : c.unknown_spans) std::println("  {:08X}-{:08X} ({} words)", lo, hi, (hi - lo) / 4);
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

}  // namespace

int main(int argc, char** argv) {
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
        return usage();
    } catch (const Failure& f) {
        std::println(stderr, "qilin: {}", f.message);
        return 1;
    } catch (const std::exception& e) {
        std::println(stderr, "qilin: internal error: {}", e.what());
        return 1;
    }
}
