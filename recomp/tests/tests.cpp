// Self-contained tests: SHA-256, a synthetic DOL built in memory, the
// Broadway decoder, and function discovery on a synthetic program. No game files required (runs in CI).
#include <algorithm>
#include <cstring>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "cfg.hpp"
#include "decoder.hpp"
#include "dol.hpp"
#include "sha256.hpp"

using namespace qilin;

namespace {

int failures = 0;
#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::println(stderr, "{}:{}: CHECK failed: {}", __FILE__, __LINE__, #cond); \
            ++failures;                                                                 \
        }                                                                               \
    } while (0)

void put_be32(std::vector<uint8_t>& d, size_t o, uint32_t v) {
    d[o] = static_cast<uint8_t>(v >> 24);
    d[o + 1] = static_cast<uint8_t>(v >> 16);
    d[o + 2] = static_cast<uint8_t>(v >> 8);
    d[o + 3] = static_cast<uint8_t>(v);
}

// ---------------------------------------------------------------- SHA-256

std::vector<uint8_t> pattern(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 7 + 3);
    return v;
}

void test_sha256() {
    CHECK(sha256_hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string_view abc = "abc";
    CHECK(sha256_hex({reinterpret_cast<const uint8_t*>(abc.data()), abc.size()}) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // Padding edges: the length fits in the last block, just doesn't, and exact blocks.
    CHECK(sha256_hex(pattern(55)) == "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b");
    CHECK(sha256_hex(pattern(56)) == "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27");
    CHECK(sha256_hex(pattern(63)) == "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055");
    CHECK(sha256_hex(pattern(64)) == "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241");
    CHECK(sha256_hex(pattern(119)) == "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e");
    CHECK(sha256_hex(pattern(1000)) == "1e9bc38cbf860b9ec31918b065f9b52476c549a782e0e7990bed8ce3868d2371");
    const std::vector<uint8_t> million(1000000, 'a');
    CHECK(sha256_hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// ---------------------------------------------------------------- DOL

// A synthetic DOL: code at 0x80003100, data at 0x80005000, a small-data
// section inside the BSS range (as Wii linkers lay them out), BSS 0x80006000-0x80007000.
struct SyntheticDol {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x400, 0);

    SyntheticDol() {
        section(0, 0x100, 0x80003100, 0x100);   // text0
        section(7, 0x200, 0x80005000, 0x100);   // data0
        section(8, 0x300, 0x80006800, 0x40);    // data1, inside BSS
        put_be32(bytes, 0xD8, 0x80006000);      // BSS
        put_be32(bytes, 0xDC, 0x1000);
        put_be32(bytes, 0xE0, 0x80003100);      // entry
        for (size_t i = 0x100; i < 0x200; i += 4) put_be32(bytes, i, 0x60000000);  // ori r0,r0,0
        put_be32(bytes, 0x100, 0x38600001);     // addi r3,0,1
        for (size_t i = 0x200; i < 0x340; ++i) bytes[i] = static_cast<uint8_t>(i);  // data0, data1
    }
    void section(unsigned slot, uint32_t offset, uint32_t address, uint32_t size) {
        put_be32(bytes, 4 * slot, offset);
        put_be32(bytes, 0x48 + 4 * slot, address);
        put_be32(bytes, 0x90 + 4 * slot, size);
    }
};

void test_dol() {
    SyntheticDol s;
    auto dol = Dol::parse(s.bytes);
    CHECK(dol.has_value());
    if (!dol) {
        std::println(stderr, "  {}", dol.error());
        return;
    }
    CHECK(dol->sections.size() == 3);
    CHECK(dol->sections[0].text && dol->sections[0].index == 0 && dol->sections[0].name() == "text0");
    CHECK(!dol->sections[1].text && dol->sections[1].name() == "data0");
    CHECK(dol->sections[2].name() == "data1" && dol->sections[2].address == 0x80006800);
    CHECK(dol->entry == 0x80003100);
    CHECK(dol->bss_address == 0x80006000 && dol->bss_size == 0x1000);
    CHECK(dol->base() == 0x80003100 && dol->image_end() == 0x80007000);
    CHECK(dol->u32(0x80003100) == 0x38600001);
    CHECK(dol->u32(0x80003104) == 0x60000000);
    CHECK(dol->u8(0x80005001) == 0x01);       // data0's second byte, file offset 0x201
    CHECK(dol->u8(0x80006000) == 0);           // BSS
    CHECK(dol->u8(0x80006801) == 0x01);       // data1 inside BSS keeps its bytes (offset 0x301)
    CHECK(dol->u8(0x80006840) == 0);           // BSS after data1
    CHECK(dol->u8(0x80004000) == 0);           // gap between sections
    CHECK(dol->mapped(0x80006FFC, 4) && !dol->mapped(0x80006FFD, 4) && !dol->mapped(0x800030FF));
    CHECK(dol->section_at(0x800031FC) == &dol->sections[0]);
    CHECK(dol->section_at(0x80003200) == nullptr);
    CHECK(dol->section_at(0x80005050) == &dol->sections[1]);
    CHECK(dol->text_section_at(0x80005050) == nullptr);
    CHECK(dol->text_section_at(0x80003100) != nullptr);
    CHECK(dol->file_size == 0x400);
    CHECK(dol->sha256 == sha256_hex(s.bytes));
    CHECK(known_build(dol->sha256) == nullptr);
    CHECK(known_build("6ae3388ff644758f030bb28de28f898885dc7c2da76bff082faae9eb60e66137") != nullptr);
    bool threw = false;
    try {
        (void)dol->u32(0x80007000);
    } catch (const DolError&) {
        threw = true;
    }
    CHECK(threw);
    const std::string json = to_json(*dol);
    CHECK(json.contains("\"entry\": \"0x80003100\""));
    CHECK(json.contains("\"name\": \"data1\""));
    CHECK(json.contains("\"build\": null"));

    // Malformed files are error messages, not crashes.
    auto expect_error = [](std::vector<uint8_t> bytes, std::string_view what) {
        auto r = Dol::parse(bytes);
        CHECK(!r.has_value());
        if (!r) {
            if (!r.error().contains(what)) std::println(stderr, "  unexpected error: {}", r.error());
            CHECK(r.error().contains(what));
        }
    };
    expect_error(std::vector<uint8_t>(0x80, 0), "shorter than");
    expect_error(std::vector<uint8_t>(0x100, 0), "no sections");
    {
        SyntheticDol b;
        put_be32(b.bytes, 0x90, 0x400);  // text0 runs past the end of the file
        expect_error(b.bytes, "outside the file");
    }
    {
        SyntheticDol b;
        put_be32(b.bytes, 0x48, 0x7FFFFF00);  // text0 below MEM1
        expect_error(b.bytes, "outside MEM1");
    }
    {
        SyntheticDol b;
        put_be32(b.bytes, 0x48, 0x817FFFC0);  // text0 runs past the end of MEM1
        expect_error(b.bytes, "outside MEM1");
    }
    {
        SyntheticDol b;
        put_be32(b.bytes, 0x48 + 4 * 7, 0x80003180);  // data0 overlaps text0
        expect_error(b.bytes, "overlap");
    }
    {
        SyntheticDol b;
        put_be32(b.bytes, 0x48, 0x80003102);  // misaligned code
        expect_error(b.bytes, "word-aligned");
    }
    {
        SyntheticDol b;
        put_be32(b.bytes, 0xE0, 0x80005000);  // entry in data
        expect_error(b.bytes, "entry point");
    }
    {
        SyntheticDol b;
        put_be32(b.bytes, 0xD8, 0x81800000);  // BSS past MEM1
        expect_error(b.bytes, "BSS");
    }
}

// ---------------------------------------------------------------- decoder

struct Case {
    uint32_t pc, word;
    std::string_view text;
};

// Expected text from GNU objdump 2.42 `-M broadway,raw` (mftb excepted: see
// tools/check/verify_decoder.py). The sweep in CI checks millions more.
constexpr Case kCases[] = {
    {0x80003100, 0x38610008, "addi r3,r1,8"},
    {0x80003104, 0x38600000, "addi r3,0,0"},
    {0x80003108, 0x3C608000, "addis r3,0,-32768"},
    {0x8000310C, 0x3863FFFC, "addi r3,r3,-4"},
    {0x80003110, 0x80010014, "lwz r0,20(r1)"},
    {0x80003114, 0x8061FFF8, "lwz r3,-8(r1)"},
    {0x80003118, 0x9421FFE0, "stwu r1,-32(r1)"},
    {0x8000311C, 0x7C0802A6, "mfspr r0,8"},
    {0x80003120, 0x7C0803A6, "mtspr 8,r0"},
    {0x80003124, 0x7C6C42E6, "mftb r3,268"},
    {0x80003128, 0x4E800020, "bclr 20,lt,0"},
    {0x8000312C, 0x4E800421, "bcctrl 20,lt,0"},
    {0x80003130, 0x48000010, "b 0x80003140"},
    {0x80003134, 0x4BFFFFF1, "bl 0x80003124"},
    {0x80003138, 0x48000102, "ba 0x100"},
    {0x8000313C, 0x4182FFF0, "bc 12,eq,0x8000312c"},
    {0x80003140, 0x409E000C, "bc 4,4*cr7+eq,0x8000314c"},
    {0x80003144, 0x5483103A, "rlwinm r3,r4,2,0,29"},
    {0x80003148, 0x7C632214, "add r3,r3,r4"},
    {0x8000314C, 0x7C632615, "addo. r3,r3,r4"},
    {0x80003150, 0x7C032000, "cmp cr0,0,r3,r4"},
    {0x80003154, 0x2C030000, "cmpi cr0,0,r3,0"},
    {0x80003158, 0x2807FFFF, "cmpli cr0,0,r7,65535"},
    {0x8000315C, 0x4C063342, "crorc lt,4*cr1+eq,4*cr1+eq"},
    {0x80003160, 0x4C840000, "mcrf cr1,cr1"},
    {0x80003164, 0xFC20101E, "fctiwz f1,f2"},
    {0x80003168, 0xEC22182A, "fadds f1,f2,f3"},
    {0x8000316C, 0xFC2208FA, "fmadd f1,f2,f3,f1"},
    {0x80003170, 0xEC201830, "fres f1,f3,0"},
    {0x80003174, 0xFF80210C, "mtfsfi 7,2"},
    {0x80003178, 0xFDFE0D8E, "mtfsf 255,f1"},
    {0x8000317C, 0xFC20048F, "mffs. f1"},
    {0x80003180, 0xE0210008, "psq_l f1,8(r1),0,0"},
    {0x80003184, 0xF0A3FFF8, "psq_st f5,-8(r3),1,7"},
    {0x80003188, 0xE4C1C014, "psq_lu f6,20(r1),1,4"},
    {0x8000318C, 0x1062000C, "psq_lx f3,r2,r0,0,0"},
    {0x80003190, 0x1040208E, "psq_stx f2,r0,r4,0,1"},
    {0x80003194, 0x10821814, "ps_sum0 f4,f2,f0,f3"},
    {0x80003198, 0x102310FA, "ps_madd f1,f3,f3,f2"},
    {0x8000319C, 0x10642420, "ps_merge00 f3,f4,f4"},
    {0x800031A0, 0x10011040, "ps_cmpo0 cr0,f1,f2"},
    {0x800031A4, 0x100307EC, "dcbz_l r3,r0"},
    {0x800031A8, 0x7C6004AA, "lswi r3,0,32"},
    {0x800031AC, 0x44000002, "sc 0"},
    {0x800031B0, 0x7C0004AC, "sync 0"},
    {0x800031B4, 0x4C00012C, "isync"},
    {0x800031B8, 0x7C0006AC, "eieio"},
    {0x800031BC, 0x4C000064, "rfi"},
    {0x800031C0, 0x7C6FF120, "mtcrf 255,r3"},
    {0x800031C4, 0x7C64292D, "stwcx. r3,r4,r5"},
    {0x800031C8, 0x7C642828, "lwarx r3,r4,r5,0"},
    {0x800031CC, 0x7C002264, "tlbie r4,0"},
    {0x800031D0, 0x7C0018AC, "dcbf 0,r3,0"},
    {0x800031D4, 0x0C9F0000, "twi 4,r31,0"},
    {0x800031D8, 0x7FE00008, "tw 31,r0,r0"},
    {0x800031DC, 0x7C631670, "srawi r3,r3,2"},
    {0x800031E0, 0x7C640734, "extsh r4,r3"},
    {0x800031E4, 0x7C6003A6, "mtspr 0,r3"},
    {0x800031E8, 0xBBC1FFF8, "lmw r30,-8(r1)"},
    {0x800031EC, 0x7C8004A6, "mfsr r4,0"},
    {0x800031F0, 0x7C8401A4, "mtsr 4,r4"},
    {0x800031F4, 0x57E3063E, "rlwinm r3,r31,0,24,31"},
    {0x800031F8, 0x7D435378, "or r3,r10,r10"},
};

// Words that must not decode, and why.
struct Bad {
    uint32_t word;
    std::string_view why;
};
constexpr Bad kInvalid[] = {
    {0x00000000, "primary opcode 0"},
    {0x84630000, "lwzu r3,0(r3): base is the target"},
    {0x84600000, "lwzu r3,0(0): no base"},
    {0x94600000, "stwu r3,0(0): update with rA=0"},
    {0x40E00000, "bc with BO 0b00111: a z bit set"},
    {0x4E000420, "bcctr with BO 0b10000: decrements CTR"},
    {0x2C230000, "cmpi with L=1"},
    {0xFC20102C, "fsqrt: not on the 750CL"},
    {0xE4600000, "psq_lu with rA=0"},
    {0x7C641C2A, "lswx r3,r4,r3: rD = rB"},
    {0x7C83202F, "lwzx with the record bit set"},
    {0x7C632416, "mulhwu with bit 21 (OE) set"},
    {0x7C6304AA, "lswi r3,r3,32: rA among the registers it loads"},
    {0xBB9E0000, "lmw r28,0(r30): base among the loaded registers"},
    {0x44000003, "sc with bit 31 set"},
    {0x7C000265, "tlbie with the record bit set"},
    {0x7C4004AC, "sync with bit 9 set"},
    {0x7C0002E4, "tlbia: not on the 750CL"},
};

// One valid word per instruction: the opcode bits plus operand fields taken
// from a template chosen to satisfy every operand constraint (rD/BO/TO = 20,
// rA = 4, rB/NB = 5, frC = 6).
uint32_t canonical(const ppc::Spec& s) {
    constexpr uint32_t kTemplate = (20u << 21) | (4u << 16) | (5u << 11) | (6u << 6) | (7u << 1);
    uint32_t fields = 0;
    for (ppc::Opnd o : s.operands) fields |= ppc::field_mask(o);
    return s.match | (kTemplate & fields & ~s.mask);
}

void test_decoder() {
    for (const Case& c : kCases) {
        const ppc::Insn in = ppc::decode(c.word);
        const std::string text = ppc::disasm(in, c.pc);
        if (text != c.text) std::println(stderr, "  {:08x}: got '{}', want '{}'", c.word, text, c.text);
        CHECK(text == c.text);
    }
    for (const Bad& b : kInvalid) {
        const bool invalid = !ppc::decode(b.word).valid();
        if (!invalid) std::println(stderr, "  {:08x} decoded as {} ({})", b.word, ppc::disasm(ppc::decode(b.word), 0), b.why);
        CHECK(invalid);
    }

    // Every instruction decodes from its canonical word, and setting any one
    // bit it doesn't define (a reserved bit) makes the word invalid.
    CHECK(ppc::specs().size() == static_cast<size_t>(ppc::Op::count_) - 1);
    size_t checked = 0;
    for (const ppc::Spec& s : ppc::specs()) {
        const uint32_t word = canonical(s);
        const ppc::Insn in = ppc::decode(word);
        if (in.op != s.op) std::println(stderr, "  {}: canonical word {:08x} decodes as {}", s.name, word, ppc::spec(in.op).name);
        CHECK(in.op == s.op);
        CHECK(ppc::spec(s.op).name == s.name);
        uint32_t defined = s.mask;
        for (ppc::Opnd o : s.operands) defined |= ppc::field_mask(o);
        if (s.flags & (ppc::kRc | ppc::kLk)) defined |= 1;
        if (s.flags & ppc::kOe) defined |= 0x400;
        if (s.flags & ppc::kAa) defined |= 2;
        for (unsigned bit = 0; bit < 32; ++bit) {
            if (defined & (1u << bit)) continue;
            const ppc::Insn flipped = ppc::decode(word ^ (1u << bit));
            if (flipped.valid())
                std::println(stderr, "  {}: reserved bit {} set still decodes ({:08x} -> {})", s.name, bit,
                             word ^ (1u << bit), ppc::disasm(flipped, 0));
            CHECK(!flipped.valid());
        }
        ++checked;
    }
    CHECK(checked == 222);  // the 750CL's instructions, paired singles included

    // Fields and suffixes.
    CHECK(ppc::decode(0x3863FFFC).simm() == -4);
    CHECK(ppc::decode(0x4BFFFFF1).branch_target(0x80003134) == 0x80003124);
    CHECK(ppc::decode(0x4BFFFFF3).branch_target(0x80003134) == 0xFFFFFFF0);  // bla -16
    CHECK(ppc::decode(0x4182FFF0).branch_target(0x8000313C) == 0x8000312C);
    CHECK(ppc::decode(0xF0A3FFF8).psq_d() == -8);
    CHECK(ppc::decode(0xF0A3FFF8).psq_w() == 1 && ppc::decode(0xF0A3FFF8).psq_i() == 7);
    CHECK(ppc::decode(0x1040208E).psqx_w() == 0 && ppc::decode(0x1040208E).psqx_i() == 1);
    CHECK(ppc::decode(0x7C6C42E6).spr() == 268);
    CHECK(ppc::mnemonic(ppc::decode(0x4BFFFFF3)) == "bla");
    CHECK(ppc::mnemonic(ppc::decode(0x7C632615)) == "addo.");
    CHECK(ppc::mnemonic(ppc::decode(0x4E800421)) == "bcctrl");
    CHECK(ppc::spec(ppc::Op::psq_l).flags & ppc::kPaired);
    CHECK(ppc::spec(ppc::Op::mtmsr).flags & ppc::kPrivileged);
    CHECK(ppc::spec(ppc::Op::bc).flags & ppc::kBranch);
    CHECK(ppc::disasm(ppc::decode(0), 0) == ".long 0x0");
}

// ---------------------------------------------------------------- CFG

// A synthetic program with one function for each way a function is found,
// a CodeWarrior switch, a register-save chain, the get-PC idiom, an indirect
// tail call, padding and dead code. text0 0x80003100-0x80003300, data0 0x80005000.
struct Program {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x400, 0);
    static constexpr uint32_t kText = 0x80003100, kData = 0x80005000;

    void word(uint32_t a, uint32_t w) {
        put_be32(bytes, a >= kData ? 0x300 + (a - kData) : 0x100 + (a - kText), w);
    }
    static uint32_t b(uint32_t pc, uint32_t to, bool link = false) {
        return 0x48000000u | ((to - pc) & 0x03FFFFFCu) | (link ? 1u : 0u);
    }
    static uint32_t bc(uint32_t pc, unsigned bo, unsigned bi, uint32_t to) {
        return (16u << 26) | (bo << 21) | (bi << 16) | ((to - pc) & 0xFFFCu);
    }
    static uint32_t li(unsigned rd, int16_t v) { return (14u << 26) | (rd << 21) | static_cast<uint16_t>(v); }

    Program() {
        put_be32(bytes, 0x00, 0x100);  // text0
        put_be32(bytes, 0x48, kText);
        put_be32(bytes, 0x90, 0x200);
        put_be32(bytes, 0x1C, 0x300);  // data0
        put_be32(bytes, 0x64, kData);
        put_be32(bytes, 0xAC, 0x100);
        put_be32(bytes, 0xE0, kText);  // entry

        // 80003100 entry: calls, a lis/addi pointer, then a tail call.
        word(0x80003100, 0x9421FFF0);                     // stwu r1,-16(r1)
        word(0x80003104, b(0x80003104, 0x80003140, true)); // bl switch
        word(0x80003108, b(0x80003108, 0x80003180, true)); // bl save chain, first entry
        word(0x8000310C, b(0x8000310C, 0x80003184, true)); // bl save chain, second entry
        word(0x80003110, b(0x80003110, 0x800031A0, true)); // bl get-PC user
        word(0x80003114, 0x3C608000);                     // lis r3,0x8000
        word(0x80003118, 0x386331D0);                     // addi r3,r3,0x31D0: thread entry
        word(0x8000311C, 0x38210010);                     // addi r1,r1,16
        word(0x80003120, b(0x80003120, 0x800031C0));      // b: tail call
        // 80003140 switch (r3) with cases 0, 1, 2 and a default.
        word(0x80003140, 0x28030002);                     // cmplwi r3,2
        word(0x80003144, bc(0x80003144, 12, 1, 0x80003170)); // bgt default
        word(0x80003148, 0x3C808000);                     // lis r4,0x8000
        word(0x8000314C, 0x5460103A);                     // slwi r0,r3,2
        word(0x80003150, 0x38845000);                     // addi r4,r4,0x5000
        word(0x80003154, 0x7C04002E);                     // lwzx r0,r4,r0
        word(0x80003158, 0x7C0903A6);                     // mtctr r0
        word(0x8000315C, 0x4E800420);                     // bctr
        word(0x80003160, li(3, 0));
        word(0x80003164, 0x4E800020);
        word(0x80003168, li(3, 1));
        word(0x8000316C, 0x4E800020);
        word(0x80003170, li(3, -1));                      // default
        word(0x80003174, 0x4E800020);
        word(0x80003178, li(3, 2));                       // case 2, after the default
        word(0x8000317C, 0x4E800020);
        // 80003180 register-save chain: two entry points, the first falls into the second.
        word(0x80003180, 0x91CBFFF8);                     // stw r14,-8(r11)
        word(0x80003184, 0x91EBFFFC);                     // stw r15,-4(r11)
        word(0x80003188, 0x4E800020);
        // 800031A0 reads the PC: bl to the next instruction is not a call.
        word(0x800031A0, b(0x800031A0, 0x800031A4, true));
        word(0x800031A4, 0x7C6802A6);                     // mflr r3
        word(0x800031A8, 0x4E800020);
        word(0x800031C0, li(3, 5));                       // reached only by the tail call
        word(0x800031C4, 0x4E800020);
        word(0x800031D0, li(3, 6));                       // reached only through lis/addi
        word(0x800031D4, 0x4E800020);
        word(0x800031E0, li(3, 7));                       // reached only through a vtable in data
        word(0x800031E4, b(0x800031E4, 0x800031EC));
        word(0x800031E8, li(3, 9));                       // dead: nothing reaches it
        word(0x800031EC, 0x4E800020);
        word(0x80003200, li(3, 8));                       // referenced by nothing: a gap
        word(0x80003204, 0x4E800020);
        word(0x80003210, 0x81830000);                     // lwz r12,0(r3)
        word(0x80003214, 0x7D8903A6);                     // mtctr r12
        word(0x80003218, 0x4E800420);                     // bctr: an indirect tail call
        // 80003220 tail-calls a function that sets up a frame inside its own
        // bound, leaving its blr unreachable after the tail call.
        word(0x80003220, li(3, 1));
        word(0x80003224, b(0x80003224, 0x8000322C));
        word(0x80003228, 0x4E800020);                     // leftover blr: dead
        word(0x8000322C, 0x9421FFF0);                     // stwu r1,-16(r1): a function
        word(0x80003230, 0x38210010);
        word(0x80003234, 0x4E800020);
        // 80003240 ends with a call through bctr; its epilogue stays behind.
        word(0x80003240, 0x9421FFF0);
        word(0x80003244, 0x7C6903A6);                     // mtctr r3
        word(0x80003248, 0x4E800420);                     // bctr
        word(0x8000324C, 0x38210010);                     // leftover epilogue: dead
        word(0x80003250, 0x4E800020);

        word(0x80005000, 0x80003160);                     // the switch's table
        word(0x80005004, 0x80003168);
        word(0x80005008, 0x80003178);
        word(0x8000500C, 0x800031E0);                     // a vtable
        word(0x80005010, 0x80003210);
        word(0x80005014, 0x80003220);
        word(0x80005018, 0x80003240);
    }
};

void test_cfg() {
    const Program p;
    auto dol = Dol::parse(p.bytes);
    CHECK(dol.has_value());
    if (!dol) return;
    Cfg cfg(*dol);
    cfg.run();
    const auto& fs = cfg.functions();
    const std::vector<std::pair<uint32_t, Origin>> expected = {
        {0x80003100, Origin::Entry},   {0x80003140, Origin::Call},    {0x80003180, Origin::Call},
        {0x80003184, Origin::Call},    {0x800031A0, Origin::Call},    {0x800031C0, Origin::TailCall},
        {0x800031D0, Origin::ImmPtr},  {0x800031E0, Origin::DataPtr}, {0x80003200, Origin::Gap},
        {0x80003210, Origin::DataPtr}, {0x80003220, Origin::DataPtr}, {0x8000322C, Origin::TailCall},
        {0x80003240, Origin::DataPtr},
    };
    CHECK(fs.size() == expected.size());
    for (const auto& [start, origin] : expected) {
        auto it = fs.find(start);
        if (it == fs.end() || it->second.origin != origin)
            std::println(stderr, "  function {:08x}: {}", start, it == fs.end() ? "missing" : origin_name(it->second.origin));
        CHECK(it != fs.end() && it->second.origin == origin);
    }
    if (fs.size() != expected.size()) return;

    const Function& entry = fs.at(0x80003100);
    CHECK(entry.calls == (std::set<uint32_t>{0x80003140, 0x80003180, 0x80003184, 0x800031A0}));
    CHECK(entry.tails == (std::set<uint32_t>{0x800031C0}));
    CHECK(entry.insns.size() == 9);
    const Function& sw = fs.at(0x80003140);
    CHECK(sw.jtabs.size() == 1);
    if (sw.jtabs.size() == 1) {
        CHECK(sw.jtabs[0].proven);
        CHECK(sw.jtabs[0].table == 0x80005000);
        CHECK(sw.jtabs[0].entries == (std::vector<uint32_t>{0x80003160, 0x80003168, 0x80003178}));
    }
    CHECK(sw.insns.size() == 16);
    CHECK(sw.leaders.contains(0x80003178) && sw.leaders.contains(0x80003170));
    CHECK(fs.at(0x80003180).falls_into == 0x80003184 && fs.at(0x80003180).insns.size() == 1);
    CHECK(fs.at(0x800031A0).calls.empty() && fs.at(0x800031A0).insns.size() == 3);
    CHECK(fs.at(0x800031E0).insns.size() == 3);
    CHECK(fs.at(0x80003210).itails == (std::vector<uint32_t>{0x80003218}));
    CHECK(fs.at(0x80003220).tails == (std::set<uint32_t>{0x8000322C}) && fs.at(0x80003220).insns.size() == 2);
    CHECK(fs.at(0x80003240).itails == (std::vector<uint32_t>{0x80003248}));
    CHECK(cfg.suspects().empty());
    CHECK(cfg.rejected().empty());

    const auto cov = cfg.coverage();
    CHECK(cov.size() == 1);
    if (cov.size() == 1) {
        CHECK(cov[0].code == 51);
        CHECK(cov[0].dead == 4);  // 800031E8, the leftover blr at 80003228, the epilogue at 8000324C
        CHECK(cov[0].unknown == 0);
        CHECK(cov[0].padding == 128 - 55);
    }
    CHECK(cfg.to_json().contains("\"falls_into\": \"0x80003184\""));
    CHECK(percent(1, 3, 2) == "33.33%" && percent(2, 3, 0) == "67%" && percent(0, 0, 1) == "n/a");

    // Weak evidence that reaches an invalid word is rejected: a data pointer
    // to a clean boundary whose code runs into a reserved encoding.
    Program q;
    q.word(0x80003260, Program::li(3, 1));
    q.word(0x80003264, 0x7C83202F);  // lwzx with the record bit: invalid
    q.word(0x80005020, 0x80003260);
    auto dol2 = Dol::parse(q.bytes);
    CHECK(dol2.has_value());
    if (!dol2) return;
    Cfg cfg2(*dol2);
    cfg2.run();
    CHECK(!cfg2.functions().contains(0x80003260));
    CHECK(cfg2.rejected() == (std::vector<uint32_t>{0x80003260}));
    CHECK(cfg2.coverage()[0].unknown == 2);
}

// Switches found in Punch-Out!!: each case adds one function at 0x80003260,
// reached through a pointer in data (0x80005048), with its table at
// 0x80005040 (two entries). `run` returns that function's tables.
struct SwitchCase {
    std::vector<std::pair<uint32_t, uint32_t>> code;
    std::vector<uint32_t> table;
    std::vector<std::pair<uint32_t, uint32_t>> extra_data = {};
};

std::optional<Function> run_switch(const SwitchCase& c, std::vector<uint32_t>* starts = nullptr) {
    Program p;
    for (const auto& [a, w] : c.code) p.word(a, w);
    for (size_t k = 0; k < c.table.size(); ++k) p.word(0x80005040 + 4 * static_cast<uint32_t>(k), c.table[k]);
    p.word(0x80005048, 0x80003260);
    for (const auto& [a, w] : c.extra_data) p.word(a, w);
    auto dol = Dol::parse(p.bytes);
    if (!dol) return std::nullopt;
    Cfg cfg(*dol);
    cfg.run();
    if (starts)
        for (const auto& [s, f] : cfg.functions()) starts->push_back(s);
    auto it = cfg.functions().find(0x80003260);
    if (it == cfg.functions().end()) return std::nullopt;
    return it->second;
}

bool proven_two(const std::optional<Function>& f, uint32_t c0, uint32_t c1) {
    return f && f->jtabs.size() == 1 && f->jtabs[0].proven && f->jtabs[0].table == 0x80005040 &&
           f->jtabs[0].entries == std::vector<uint32_t>{c0, c1};
}

void test_cfg_switches() {
    using P = Program;
    const uint32_t blr = 0x4E800020, mtctr_r0 = 0x7C0903A6, bctr = 0x4E800420;

    // A word in data that happens to equal a case label must not make the
    // label a function: that would cut the switch's function short.
    {
        const SwitchCase c{{{0x80003260, 0x28030001},                            // cmplwi r3,1
                            {0x80003264, P::bc(0x80003264, 12, 1, 0x80003284)},  // bgt default
                            {0x80003268, 0x3C808000},                            // lis r4,0x8000
                            {0x8000326C, 0x5460103A},                            // slwi r0,r3,2
                            {0x80003270, 0x38845040},                            // addi r4,r4,0x5040
                            {0x80003274, 0x7C04002E},                            // lwzx r0,r4,r0
                            {0x80003278, mtctr_r0},
                            {0x8000327C, bctr},
                            {0x80003280, P::li(3, 0)},
                            {0x80003284, blr},
                            {0x80003288, P::li(3, 1)},
                            {0x8000328C, P::b(0x8000328C, 0x80003284)}},
                           {0x80003280, 0x80003288},
                           {{0x80005050, 0x80003288}}};  // the stray "pointer"
        std::vector<uint32_t> starts;
        const auto f = run_switch(c, &starts);
        CHECK(proven_two(f, 0x80003280, 0x80003288));
        CHECK(std::ranges::find(starts, 0x80003288u) == starts.end());
    }
    // The bound check after the shift, and the index register reused for the
    // table's address after the check.
    {
        const SwitchCase c{{{0x80003260, 0x5469103A},                            // slwi r9,r3,2
                            {0x80003264, 0x28030001},                            // cmplwi r3,1
                            {0x80003268, P::bc(0x80003268, 12, 1, 0x80003288)},  // bgt default
                            {0x8000326C, 0x3C608000},                            // lis r3,0x8000
                            {0x80003270, 0x38635040},                            // addi r3,r3,0x5040
                            {0x80003274, 0x7C63482E},                            // lwzx r3,r3,r9
                            {0x80003278, 0x7C6903A6},                            // mtctr r3
                            {0x8000327C, bctr},
                            {0x80003280, P::li(3, 0)},
                            {0x80003284, blr},
                            {0x80003288, P::li(3, 1)},
                            {0x8000328C, blr}},
                           {0x80003280, 0x80003288}};
        CHECK(proven_two(run_switch(c), 0x80003280, 0x80003288));
        // ... but not if the index changes between the shift and the check.
        SwitchCase d = c;
        d.code.insert(d.code.begin() + 1, {0x80003264, 0x38630001});  // addi r3,r3,1 (overrides the cmplwi)
        d.code.erase(d.code.begin() + 2);
        d.code.push_back({0x80003290, 0x28030001});  // keep a check, but out of line
        const auto g = run_switch(d);
        CHECK(g && (g->jtabs.empty() || !g->jtabs[0].proven));
    }
    // A loop whose table base is set (lis r31) after an earlier, unrelated use
    // of r31: constant from the loop's head on.
    const std::vector<std::pair<uint32_t, uint32_t>> loop_base = {
        {0x80003260, P::li(31, 0)},                         // li r31,0
        {0x80003264, 0x7FE3FB78},                           // mr r3,r31
        {0x80003268, 0x3FE08000},                           // lis r31,0x8000 (the region's head)
        {0x8000326C, 0x28040001},                           // cmplwi r4,1 (loop top)
        {0x80003270, P::bc(0x80003270, 12, 1, 0x80003294)}, // bgt latch
        {0x80003274, 0x5480103A},                           // slwi r0,r4,2
        {0x80003278, 0x38BF5040},                           // addi r5,r31,0x5040
        {0x8000327C, 0x7CA5002E},                           // lwzx r5,r5,r0
        {0x80003280, 0x7CA903A6},                           // mtctr r5
        {0x80003284, bctr},
        {0x80003288, P::li(3, 0)},
        {0x8000328C, P::b(0x8000328C, 0x80003294)},
        {0x80003290, P::li(3, 1)},
        {0x80003294, 0x38840001},                           // addi r4,r4,1 (latch)
        {0x80003298, 0x2C040004},                           // cmpwi r4,4
        {0x8000329C, P::bc(0x8000329C, 12, 0, 0x8000326C)}, // blt loop top
        {0x800032A0, blr}};
    CHECK(proven_two(run_switch({loop_base, {0x80003288, 0x80003290}}), 0x80003288, 0x80003290));
    {
        SwitchCase c{loop_base, {0x80003288, 0x80003290}};
        c.code.push_back({0x80003290, 0x3BE00000});  // case 1 rewrites r31 inside the loop
        const auto f = run_switch(c);
        CHECK(f && (f->jtabs.empty() || !f->jtabs[0].proven));
    }
    // A loop with a strength-reduced index: the table is indexed by r19 (+4 a
    // turn) and bounds-checked on r24 (+1 a turn).
    const std::vector<std::pair<uint32_t, uint32_t>> loop_scaled = {
        {0x80003260, P::li(24, 0)},
        {0x80003264, P::li(19, 0)},
        {0x80003268, 0x28180001},                           // cmplwi r24,1 (loop top)
        {0x8000326C, P::bc(0x8000326C, 12, 1, 0x80003290)}, // bgt latch
        {0x80003270, 0x3C608000},                           // lis r3,0x8000
        {0x80003274, 0x38635040},                           // addi r3,r3,0x5040
        {0x80003278, 0x7C63982E},                           // lwzx r3,r3,r19
        {0x8000327C, 0x7C6903A6},                           // mtctr r3
        {0x80003280, bctr},
        {0x80003284, P::li(5, 0)},
        {0x80003288, P::b(0x80003288, 0x80003290)},
        {0x8000328C, P::li(5, 1)},
        {0x80003290, 0x3B180001},                           // addi r24,r24,1
        {0x80003294, 0x3A730004},                           // addi r19,r19,4
        {0x80003298, 0x2C180007},                           // cmpwi r24,7
        {0x8000329C, P::bc(0x8000329C, 12, 0, 0x80003268)}, // blt loop top
        {0x800032A0, blr}};
    CHECK(proven_two(run_switch({loop_scaled, {0x80003284, 0x8000328C}}), 0x80003284, 0x8000328C));
    {
        SwitchCase c{loop_scaled, {0x80003284, 0x8000328C}};
        c.code.push_back({0x80003294, 0x3A730008});  // r19 steps by 8: not 4 * r24
        const auto f = run_switch(c);
        CHECK(f && f->jtabs.empty());
    }
}

}  // namespace

int main() {
    test_sha256();
    test_dol();
    test_decoder();
    test_cfg();
    test_cfg_switches();
    if (failures) {
        std::println(stderr, "{} check(s) failed", failures);
        return 1;
    }
    std::println("all tests passed");
    return 0;
}
