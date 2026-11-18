#include "lift.hpp"

#include <algorithm>
#include <format>
#include <map>

namespace qilin {

using ppc::Insn;
using ppc::Op;

std::string fn_name(uint32_t address) { return std::format("fn_{:08X}", address); }

namespace {

constexpr unsigned kXer = 1, kLr = 8, kCtr = 9, kGqr0 = 912;

// The mask rlwinm and friends build from MB and ME (IBM bit numbering).
uint32_t rot_mask(unsigned mb, unsigned me) {
    const uint32_t from = 0xFFFFFFFFu >> mb;
    const uint32_t to = me == 31 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1));
    return mb <= me ? (from & to) : (from | to);
}

std::string hex(uint32_t v) { return std::format("0x{:08X}u", v); }

// " + 16u" or " - 16u": an offset added to an address, as a reader would write it.
std::string plus(int32_t v) {
    return v < 0 ? std::format(" - {}u", 0u - static_cast<uint32_t>(v)) : std::format(" + {}u", static_cast<uint32_t>(v));
}

}  // namespace

bool in_slice(const Insn& in, std::string* why) {
    auto no = [&](std::string w) {
        if (why) *why = std::move(w);
        return false;
    };
    if (!in.valid()) return no("an invalid word");
    const ppc::Spec& s = ppc::spec(in.op);
    if (s.flags & ppc::kPaired) return no("paired singles");
    if (s.flags & ppc::kFloat) return no("floating point");
    if (s.flags & ppc::kPrivileged) return no(std::format("privileged ({})", s.name));
    if ((s.flags & ppc::kOe) && in.oe()) return no(std::format("overflow-enable form ({}o)", s.name));
    switch (in.op) {
    case Op::lswi: case Op::lswx: case Op::stswi: case Op::stswx: return no("string load/store");
    case Op::sc: return no("system call");
    case Op::mftb: return no("time base");
    case Op::eciwx: case Op::ecowx: return no("external control");
    case Op::mfspr: case Op::mtspr: {
        const unsigned n = in.spr();
        if (n == kXer || n == kLr || n == kCtr || (n >= kGqr0 && n < kGqr0 + 8)) return true;
        return no(std::format("special-purpose register {}", n));
    }
    default: return true;
    }
}

namespace {

class Emitter {
public:
    explicit Emitter(const LiftInput& f) : f_(f) {
        for (const auto& [a, in] : f.insns) present_.insert(a);
        for (const JumpTable& t : f.jtabs) tables_[t.at] = &t;
    }

    Lifted run() {
        Lifted out;
        for (const auto& [a, in] : f_.insns) {
            std::string why;
            if (!in_slice(in, &why)) {
                out.why = why;
                out.why_at = a;
                return out;
            }
        }
        // Labels: every branch target inside the function.
        for (const auto& [a, in] : f_.insns) {
            if ((in.op == Op::b || in.op == Op::bc) && !(in.lk() && f_.not_calls.contains(a))) {
                const uint32_t t = in.branch_target(a);
                if (present_.contains(t) && !in.lk()) labels_.insert(t);
            }
            if (auto it = tables_.find(a); it != tables_.end())
                for (uint32_t t : it->second->targets) labels_.insert(t);
        }
        std::string body;
        for (size_t k = 0; k < f_.insns.size(); ++k) {
            const auto& [a, in] = f_.insns[k];
            if (labels_.contains(a)) body += std::format("L_{:08X}:\n", a);
            std::string line;
            if (!insn(a, in, line)) {
                out.why = error_;
                out.why_at = a;
                return out;
            }
            body += line;
            const bool last = k + 1 == f_.insns.size();
            const uint32_t next = last ? 0 : f_.insns[k + 1].first;
            if (!Cfg::is_terminator(in) && !(tables_.contains(a)) && (last || next != a + 4)) {
                // Execution continues past this instruction into code that isn't the next one we have.
                if (f_.falls_into == a + 4) {
                    body += std::format("    {} {}(c); return;\n", sync_out(), fn_name(a + 4));
                    callees_.insert(a + 4);
                } else {
                    out.why = std::format("execution runs on to {:08X}", a + 4);
                    out.why_at = a;
                    return out;
                }
            }
        }
        out.code = std::format("void {}(Cpu& c) {{\n{}{}}}\n", fn_name(f_.start), prologue(), body);
        out.ok = true;
        return out;
    }

private:
    // Register uses, recorded as the body is emitted.
    std::string R(unsigned n) {
        gprs_.insert(n);
        return std::format("r{}", n);
    }
    std::string RA0(unsigned n) { return n == 0 ? "0u" : R(n); }
    std::string CR(unsigned n) {
        crs_.insert(n);
        return std::format("cr{}", n);
    }
    std::string SO() { return use("so"); }
    std::string OV() { return use("ov"); }
    std::string CA() { return use("ca"); }
    std::string LR() { return use("lr"); }
    std::string CTR() { return use("ctr"); }
    std::string use(const char* n) {
        specials_.insert(n);
        return n;
    }

    std::string prologue() {
        std::string p;
        for (unsigned n : gprs_) p += std::format("    uint32_t r{0} = c.r[{0}];\n", n);
        for (unsigned n : crs_) p += std::format("    uint32_t cr{0} = (c.cr >> {1}) & 15u;\n", n, 28 - 4 * n);
        if (specials_.contains("lr")) p += "    uint32_t lr = c.lr;\n";
        if (specials_.contains("ctr")) p += "    uint32_t ctr = c.ctr;\n";
        if (specials_.contains("so")) p += "    uint32_t so = c.xer_so;\n";
        if (specials_.contains("ov")) p += "    uint32_t ov = c.xer_ov;\n";
        if (specials_.contains("ca")) p += "    uint32_t ca = c.xer_ca;\n";
        return p;
    }
    // Written back before anything outside the function can see the state
    // (calls, returns, traps); read again after a call. The text is a
    // placeholder until the function's uses are known.
    std::string sync_out() { return "@OUT@"; }
    std::string sync_in() { return "@IN@"; }

public:
    std::string expand(const std::string& code) const {
        std::string out_s, in_s;
        for (unsigned n : gprs_) {
            out_s += std::format("c.r[{0}] = r{0}; ", n);
            in_s += std::format("r{0} = c.r[{0}]; ", n);
        }
        if (!crs_.empty()) {
            uint32_t keep = 0xFFFFFFFFu;
            std::string merged;
            for (unsigned n : crs_) {
                keep &= ~(15u << (28 - 4 * n));
                merged += std::format(" | (cr{} << {})", n, 28 - 4 * n);
                in_s += std::format("cr{0} = (c.cr >> {1}) & 15u; ", n, 28 - 4 * n);
            }
            out_s += std::format("c.cr = (c.cr & {}){}; ", hex(keep), merged);
        }
        for (const char* s : {"lr", "ctr"})
            if (specials_.contains(s)) {
                out_s += std::format("c.{0} = {0}; ", s);
                in_s += std::format("{0} = c.{0}; ", s);
            }
        for (const char* s : {"so", "ov", "ca"})
            if (specials_.contains(s)) {
                out_s += std::format("c.xer_{0} = {0}; ", s);
                in_s += std::format("{0} = c.xer_{0}; ", s);
            }
        std::string r = code;
        for (auto [ph, with] : {std::pair{std::string("@OUT@"), out_s}, std::pair{std::string("@IN@"), in_s}})
            for (size_t p; (p = r.find(ph)) != std::string::npos;) r.replace(p, ph.size(), with);
        return r;
    }

private:
    // The condition of a bc/bclr/bcctr, after decrementing CTR if BO says so.
    std::string condition(const Insn& in, std::string& pre) {
        const unsigned bo = in.rd(), bi = in.ra();
        std::vector<std::string> parts;
        if (!(bo & 4)) {
            pre += std::format("{0} -= 1u; ", CTR());
            parts.push_back((bo & 2) ? "ctr == 0u" : "ctr != 0u");
        }
        if (!(bo & 0x10)) {
            const unsigned m = 8u >> (bi & 3);
            const std::string field = CR(bi >> 2);
            parts.push_back((bo & 8) ? std::format("({} & {}u)", field, m) : std::format("!({} & {}u)", field, m));
        }
        if (parts.empty()) return "true";
        std::string c = parts[0];
        for (size_t k = 1; k < parts.size(); ++k) c += " && " + parts[k];
        return c;
    }

    // A transfer to `target` that leaves the function: a tail call.
    bool tail_to(uint32_t target, std::string& out) {
        if (!f_.is_function || !f_.is_function(target)) {
            error_ = std::format("branch to {:08X}, which is neither in the function nor a function", target);
            return false;
        }
        callees_.insert(target);
        out = std::format("{} {}(c); return;", sync_out(), fn_name(target));
        return true;
    }
    std::string call_to(uint32_t target) {
        callees_.insert(target);
        return std::format("{} {}(c); {}", sync_out(), fn_name(target), sync_in());
    }

    std::string trap(uint32_t pc, const char* why) {
        return std::format("{} c.hooks->trap(c, {}, \"{}\");", sync_out(), hex(pc), why);
    }

    bool insn(uint32_t pc, const Insn& in, std::string& out) {
        const unsigned d = in.rd(), a = in.ra(), b = in.rb();
        const bool record_form = (ppc::spec(in.op).flags & ppc::kRc) && in.rc();
        auto line = [&](std::string s) { out = "    " + s + "\n"; };
        // The result register, then the record form on it.
        auto arith = [&](unsigned dst, std::string expr) {
            std::string s = std::format("{} = {};", R(dst), expr);
            if (record_form) s += std::format(" {} = record(r{}, {});", CR(0), dst, SO());
            line(s);
        };
        auto ea_d = [&]() { return a ? R(a) + plus(in.simm()) : std::format("{}u", static_cast<uint32_t>(in.simm())); };
        auto ea_x = [&]() { return std::format("{} + {}", RA0(a), R(b)); };
        switch (in.op) {
        // ---------------------------------------------------------------- integer arithmetic
        case Op::addi: arith(d, a ? R(a) + plus(in.simm()) : std::format("{}u", static_cast<uint32_t>(in.simm()))); return true;
        case Op::addis: arith(d, std::format("{} + {}u", RA0(a), static_cast<uint32_t>(in.simm()) << 16)); return true;
        case Op::add: arith(d, std::format("{} + {}", R(a), R(b))); return true;
        case Op::subf: arith(d, std::format("{} - {}", R(b), R(a))); return true;
        case Op::neg: arith(d, std::format("0u - {}", R(a))); return true;
        case Op::addic: arith(d, std::format("add_carry({}, {}u, 0u, {})", R(a), static_cast<uint32_t>(in.simm()), CA())); return true;
        case Op::addic_rc:
            line(std::format("{0} = add_carry({1}, {2}u, 0u, {3}); {4} = record({0}, {5});", R(d), R(a),
                             static_cast<uint32_t>(in.simm()), CA(), CR(0), SO()));
            return true;
        case Op::addc: arith(d, std::format("add_carry({}, {}, 0u, {})", R(a), R(b), CA())); return true;
        case Op::adde: arith(d, std::format("add_carry({}, {}, {}, {})", R(a), R(b), CA(), CA())); return true;
        case Op::addme: arith(d, std::format("add_carry({}, 0xFFFFFFFFu, {}, {})", R(a), CA(), CA())); return true;
        case Op::addze: arith(d, std::format("add_carry({}, 0u, {}, {})", R(a), CA(), CA())); return true;
        case Op::subfc: arith(d, std::format("add_carry(~{}, {}, 1u, {})", R(a), R(b), CA())); return true;
        case Op::subfe: arith(d, std::format("add_carry(~{}, {}, {}, {})", R(a), R(b), CA(), CA())); return true;
        case Op::subfic: arith(d, std::format("add_carry(~{}, {}u, 1u, {})", R(a), static_cast<uint32_t>(in.simm()), CA())); return true;
        case Op::subfme: arith(d, std::format("add_carry(~{}, 0xFFFFFFFFu, {}, {})", R(a), CA(), CA())); return true;
        case Op::subfze: arith(d, std::format("add_carry(~{}, 0u, {}, {})", R(a), CA(), CA())); return true;
        case Op::mulli: arith(d, std::format("{} * {}u", R(a), static_cast<uint32_t>(in.simm()))); return true;
        case Op::mullw: arith(d, std::format("{} * {}", R(a), R(b))); return true;
        case Op::mulhw: arith(d, std::format("mulhw({}, {})", R(a), R(b))); return true;
        case Op::mulhwu: arith(d, std::format("mulhwu({}, {})", R(a), R(b))); return true;
        case Op::divw: arith(d, std::format("divw({}, {})", R(a), R(b))); return true;
        case Op::divwu: arith(d, std::format("divwu({}, {})", R(a), R(b))); return true;
        // ---------------------------------------------------------------- compare
        case Op::cmp: line(std::format("{} = compare_signed(static_cast<int32_t>({}), static_cast<int32_t>({}), {});", CR(in.crfd()), R(a), R(b), SO())); return true;
        case Op::cmpl: line(std::format("{} = compare_unsigned({}, {}, {});", CR(in.crfd()), R(a), R(b), SO())); return true;
        case Op::cmpi: line(std::format("{} = compare_signed(static_cast<int32_t>({}), {}, {});", CR(in.crfd()), R(a), in.simm(), SO())); return true;
        case Op::cmpli: line(std::format("{} = compare_unsigned({}, {}u, {});", CR(in.crfd()), R(a), in.uimm(), SO())); return true;
        // ---------------------------------------------------------------- logical (rA = op(rS, ...))
        case Op::and_: arith(a, std::format("{} & {}", R(d), R(b))); return true;
        case Op::andc: arith(a, std::format("{} & ~{}", R(d), R(b))); return true;
        case Op::or_: arith(a, std::format("{} | {}", R(d), R(b))); return true;
        case Op::orc: arith(a, std::format("{} | ~{}", R(d), R(b))); return true;
        case Op::xor_: arith(a, std::format("{} ^ {}", R(d), R(b))); return true;
        case Op::nor: arith(a, std::format("~({} | {})", R(d), R(b))); return true;
        case Op::nand: arith(a, std::format("~({} & {})", R(d), R(b))); return true;
        case Op::eqv: arith(a, std::format("~({} ^ {})", R(d), R(b))); return true;
        case Op::andi_rc: line(std::format("{0} = {1} & {2}u; {3} = record({0}, {4});", R(a), R(d), in.uimm(), CR(0), SO())); return true;
        case Op::andis_rc: line(std::format("{0} = {1} & {2}u; {3} = record({0}, {4});", R(a), R(d), in.uimm() << 16, CR(0), SO())); return true;
        case Op::ori: arith(a, std::format("{} | {}u", R(d), in.uimm())); return true;
        case Op::oris: arith(a, std::format("{} | {}u", R(d), in.uimm() << 16)); return true;
        case Op::xori: arith(a, std::format("{} ^ {}u", R(d), in.uimm())); return true;
        case Op::xoris: arith(a, std::format("{} ^ {}u", R(d), in.uimm() << 16)); return true;
        case Op::cntlzw: arith(a, std::format("static_cast<uint32_t>(std::countl_zero({}))", R(d))); return true;
        case Op::extsb: arith(a, std::format("static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>({})))", R(d))); return true;
        case Op::extsh: arith(a, std::format("static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>({})))", R(d))); return true;
        // ---------------------------------------------------------------- rotate and shift
        case Op::rlwinm: arith(a, std::format("std::rotl({}, {}) & {}", R(d), b, hex(rot_mask(in.mb(), in.me())))); return true;
        case Op::rlwnm: arith(a, std::format("std::rotl({}, static_cast<int>({} & 31u)) & {}", R(d), R(b), hex(rot_mask(in.mb(), in.me())))); return true;
        case Op::rlwimi: {
            const uint32_t m = rot_mask(in.mb(), in.me());
            arith(a, std::format("(std::rotl({}, {}) & {}) | ({} & {})", R(d), b, hex(m), R(a), hex(~m)));
            return true;
        }
        case Op::slw: arith(a, std::format("slw({}, {})", R(d), R(b))); return true;
        case Op::srw: arith(a, std::format("srw({}, {})", R(d), R(b))); return true;
        case Op::sraw: arith(a, std::format("sraw({}, {}, {})", R(d), R(b), CA())); return true;
        case Op::srawi: arith(a, std::format("sraw({}, {}u, {})", R(d), b, CA())); return true;
        // ---------------------------------------------------------------- loads and stores
        case Op::lbz: arith(d, std::format("read8(c, {})", ea_d())); return true;
        case Op::lhz: arith(d, std::format("read16(c, {})", ea_d())); return true;
        case Op::lha: arith(d, std::format("static_cast<uint32_t>(static_cast<int16_t>(read16(c, {})))", ea_d())); return true;
        case Op::lwz: arith(d, std::format("read32(c, {})", ea_d())); return true;
        case Op::lbzx: arith(d, std::format("read8(c, {})", ea_x())); return true;
        case Op::lhzx: arith(d, std::format("read16(c, {})", ea_x())); return true;
        case Op::lhax: arith(d, std::format("static_cast<uint32_t>(static_cast<int16_t>(read16(c, {})))", ea_x())); return true;
        case Op::lwzx: arith(d, std::format("read32(c, {})", ea_x())); return true;
        case Op::lhbrx: arith(d, std::format("std::byteswap(static_cast<uint16_t>(read16(c, {})))", ea_x())); return true;
        case Op::lwbrx: arith(d, std::format("std::byteswap(read32(c, {}))", ea_x())); return true;
        case Op::lbzu: case Op::lhzu: case Op::lhau: case Op::lwzu:
        case Op::lbzux: case Op::lhzux: case Op::lhaux: case Op::lwzux: {
            const bool x = in.op == Op::lbzux || in.op == Op::lhzux || in.op == Op::lhaux || in.op == Op::lwzux;
            const std::string ea = x ? std::format("{} + {}", R(a), R(b)) : R(a) + plus(in.simm());
            std::string load;
            switch (in.op) {
            case Op::lbzu: case Op::lbzux: load = "read8(c, ea)"; break;
            case Op::lhzu: case Op::lhzux: load = "read16(c, ea)"; break;
            case Op::lhau: case Op::lhaux: load = "static_cast<uint32_t>(static_cast<int16_t>(read16(c, ea)))"; break;
            default: load = "read32(c, ea)"; break;
            }
            line(std::format("{{ const uint32_t ea = {}; {} = {}; {} = ea; }}", ea, R(d), load, R(a)));
            return true;
        }
        case Op::stb: line(std::format("write8(c, {}, {});", ea_d(), R(d))); return true;
        case Op::sth: line(std::format("write16(c, {}, {});", ea_d(), R(d))); return true;
        case Op::stw: line(std::format("write32(c, {}, {});", ea_d(), R(d))); return true;
        case Op::stbx: line(std::format("write8(c, {}, {});", ea_x(), R(d))); return true;
        case Op::sthx: line(std::format("write16(c, {}, {});", ea_x(), R(d))); return true;
        case Op::stwx: line(std::format("write32(c, {}, {});", ea_x(), R(d))); return true;
        case Op::sthbrx: line(std::format("write16(c, {}, std::byteswap(static_cast<uint16_t>({})));", ea_x(), R(d))); return true;
        case Op::stwbrx: line(std::format("write32(c, {}, std::byteswap({}));", ea_x(), R(d))); return true;
        case Op::stbu: case Op::sthu: case Op::stwu: case Op::stbux: case Op::sthux: case Op::stwux: {
            const bool x = in.op == Op::stbux || in.op == Op::sthux || in.op == Op::stwux;
            const std::string ea = x ? std::format("{} + {}", R(a), R(b)) : R(a) + plus(in.simm());
            const char* w = (in.op == Op::stbu || in.op == Op::stbux) ? "write8" : (in.op == Op::sthu || in.op == Op::sthux) ? "write16" : "write32";
            line(std::format("{{ const uint32_t ea = {}; {}(c, ea, {}); {} = ea; }}", ea, w, R(d), R(a)));
            return true;
        }
        case Op::lmw: {
            std::string s = std::format("{{ const uint32_t ea = {};", ea_d());
            for (unsigned r = d; r < 32; ++r) s += std::format(" {} = read32(c, ea + {}u);", R(r), 4 * (r - d));
            line(s + " }");
            return true;
        }
        case Op::stmw: {
            std::string s = std::format("{{ const uint32_t ea = {};", ea_d());
            for (unsigned r = d; r < 32; ++r) s += std::format(" write32(c, ea + {}u, {});", 4 * (r - d), R(r));
            line(s + " }");
            return true;
        }
        case Op::lwarx:
            line(std::format("{{ const uint32_t ea = {}; {} = read32(c, ea); c.reserve = 1; c.reserved = ea; }}", ea_x(), R(d)));
            return true;
        case Op::stwcx_rc:
            line(std::format("{{ const uint32_t ea = {0}; if (c.reserve && c.reserved == ea) {{ write32(c, ea, {1}); {2} = 2u | {3}; }} "
                             "else {{ {2} = {3}; }} c.reserve = 0; }}",
                             ea_x(), R(d), CR(0), SO()));
            return true;
        case Op::dcbz: line(std::format("zero_block(c, {});", ea_x())); return true;
        case Op::dcbf: case Op::dcbst: case Op::dcbt: case Op::dcbtst: case Op::icbi:
        case Op::sync: case Op::isync: case Op::eieio:
            line("/* cache or ordering hint: nothing to do on a coherent host */");
            return true;
        // ---------------------------------------------------------------- condition register
        case Op::crand: case Op::crandc: case Op::creqv: case Op::crnand:
        case Op::crnor: case Op::cror: case Op::crorc: case Op::crxor: {
            auto bit = [&](unsigned n) { return std::format("(({} >> {}u) & 1u)", CR(n >> 2), 3 - (n & 3)); };
            const std::string x = bit(a), y = bit(b);
            std::string v;
            switch (in.op) {
            case Op::crand: v = x + " & " + y; break;
            case Op::crandc: v = x + " & (" + y + " ^ 1u)"; break;
            case Op::creqv: v = "(" + x + " ^ " + y + ") ^ 1u"; break;
            case Op::crnand: v = "(" + x + " & " + y + ") ^ 1u"; break;
            case Op::crnor: v = "(" + x + " | " + y + ") ^ 1u"; break;
            case Op::cror: v = x + " | " + y; break;
            case Op::crorc: v = x + " | (" + y + " ^ 1u)"; break;
            default: v = x + " ^ " + y; break;
            }
            const unsigned sh = 3 - (d & 3);
            const std::string f = CR(d >> 2);
            line(std::format("{{ const uint32_t v = {}; {} = ({} & {}u) | (v << {}u); }}", v, f, f, 15u & ~(1u << sh), sh));
            return true;
        }
        case Op::mcrf: line(std::format("{} = {};", CR(in.crfd()), CR(in.crfs()))); return true;
        case Op::mcrxr:
            line(std::format("{} = ({} << 3) | ({} << 2) | ({} << 1); {} = 0u; {} = 0u; {} = 0u;", CR(in.crfd()), SO(), OV(), CA(), SO(), OV(), CA()));
            return true;
        case Op::mfcr: {
            std::string v;
            for (unsigned n = 0; n < 8; ++n) v += std::format("{}({} << {})", n ? " | " : "", CR(n), 28 - 4 * n);
            line(std::format("{} = {};", R(d), v));
            return true;
        }
        case Op::mtcrf: {
            std::string s;
            for (unsigned n = 0; n < 8; ++n)
                if (in.crm() & (0x80u >> n)) s += std::format("{} = ({} >> {}) & 15u; ", CR(n), R(d), 28 - 4 * n);
            line(s.empty() ? "/* mtcrf with an empty mask */" : s);
            return true;
        }
        // ---------------------------------------------------------------- special-purpose registers
        case Op::mfspr: {
            const unsigned n = in.spr();
            if (n == kLr) line(std::format("{} = {};", R(d), LR()));
            else if (n == kCtr) line(std::format("{} = {};", R(d), CTR()));
            else if (n == kXer) {
                line(std::format("{} = ({} << 31) | ({} << 30) | ({} << 29) | (c.xer_bc & 0x7Fu);", R(d), SO(), OV(), CA()));
            } else line(std::format("{} = c.gqr[{}];", R(d), n - kGqr0));
            return true;
        }
        case Op::mtspr: {
            const unsigned n = in.spr();
            if (n == kLr) line(std::format("{} = {};", LR(), R(d)));
            else if (n == kCtr) line(std::format("{} = {};", CTR(), R(d)));
            else if (n == kXer)
                line(std::format("{0} = {4} >> 31; {1} = ({4} >> 30) & 1u; {2} = ({4} >> 29) & 1u; c.xer_bc = {4} & 0x7Fu;", SO(), OV(), CA(), "", R(d)));
            else line(std::format("c.gqr[{}] = {};", n - kGqr0, R(d)));
            return true;
        }
        // ---------------------------------------------------------------- traps
        case Op::tw: case Op::twi: {
            const unsigned to = d;
            const std::string x = R(a), y = in.op == Op::tw ? R(b) : std::format("{}u", static_cast<uint32_t>(in.simm()));
            if (to == 31) {
                line(trap(pc, "unconditional trap"));
                return true;
            }
            std::vector<std::string> c;
            if (to & 16) c.push_back(std::format("static_cast<int32_t>({}) < static_cast<int32_t>({})", x, y));
            if (to & 8) c.push_back(std::format("static_cast<int32_t>({}) > static_cast<int32_t>({})", x, y));
            if (to & 4) c.push_back(std::format("{} == {}", x, y));
            if (to & 2) c.push_back(std::format("{} < {}", x, y));
            if (to & 1) c.push_back(std::format("{} > {}", x, y));
            std::string cond = c.empty() ? "false" : c[0];
            for (size_t k = 1; k < c.size(); ++k) cond += " || " + c[k];
            line(std::format("if ({}) {{ {} }}", cond, trap(pc, "conditional trap")));
            return true;
        }
        // ---------------------------------------------------------------- branches
        case Op::b: {
            const uint32_t t = in.branch_target(pc);
            if (in.lk()) {
                if (f_.not_calls.contains(pc)) {
                    line(std::format("{} = {};", LR(), hex(pc + 4)));
                    return true;
                }
                if (!f_.is_function || !f_.is_function(t)) {
                    error_ = std::format("call to {:08X}, which isn't a known function", t);
                    return false;
                }
                line(std::format("{} = {}; {}", LR(), hex(pc + 4), call_to(t)));
                return true;
            }
            if (present_.contains(t)) {
                line(std::format("goto L_{:08X};", t));
                return true;
            }
            std::string s;
            if (!tail_to(t, s)) return false;
            line(s);
            return true;
        }
        case Op::bc: {
            const uint32_t t = in.branch_target(pc);
            std::string pre;
            const std::string cond = condition(in, pre);
            if (in.lk()) {
                pre += std::format("{} = {}; ", LR(), hex(pc + 4));
                if (t == pc + 4) {  // the get-PC idiom: only LR changes
                    line(pre);
                    return true;
                }
                if (!f_.is_function || !f_.is_function(t)) {
                    error_ = std::format("conditional call to {:08X}, which isn't a known function", t);
                    return false;
                }
                line(std::format("{}if ({}) {{ {} }}", pre, cond, call_to(t)));
                return true;
            }
            if (present_.contains(t)) {
                line(std::format("{}if ({}) goto L_{:08X};", pre, cond, t));
                return true;
            }
            std::string s;
            if (!tail_to(t, s)) return false;
            line(std::format("{}if ({}) {{ {} }}", pre, cond, s));
            return true;
        }
        case Op::bclr: {
            std::string pre;
            const std::string cond = condition(in, pre);
            if (in.lk()) {
                line(std::format("{{ const uint32_t t = {0}; {1}{0} = {2}; if ({3}) {{ {4} c.hooks->call(c, t & ~3u); {5} }} }}", LR(), pre,
                                 hex(pc + 4), cond, sync_out(), sync_in()));
                return true;
            }
            line(cond == "true" ? std::format("{}{} return;", pre, sync_out()) : std::format("{}if ({}) {{ {} return; }}", pre, cond, sync_out()));
            return true;
        }
        case Op::bcctr: {
            if (auto it = tables_.find(pc); it != tables_.end()) {
                std::string s = std::format("switch ({}) {{", CTR());
                for (uint32_t t : it->second->targets) s += std::format(" case {}: goto L_{:08X};", hex(t), t);
                s += std::format(" default: {} }}", trap(pc, "switch to an address outside its table"));
                line(s);
                return true;
            }
            std::string pre;
            const std::string cond = condition(in, pre);
            if (in.lk()) {
                line(std::format("{}{} = {}; if ({}) {{ {} c.hooks->call(c, {} & ~3u); {} }}", pre, LR(), hex(pc + 4), cond, sync_out(), CTR(), sync_in()));
                return true;
            }
            line(std::format("{}if ({}) {{ {} c.hooks->call(c, {} & ~3u); return; }}", pre, cond, sync_out(), CTR()));
            return true;
        }
        default:
            error_ = std::format("{} isn't handled by the lifter", ppc::spec(in.op).name);
            return false;
        }
    }

    const LiftInput& f_;
    std::set<uint32_t> present_, labels_, callees_;
    std::map<uint32_t, const JumpTable*> tables_;
    std::set<unsigned> gprs_, crs_;
    std::set<std::string> specials_;
    std::string error_;
};

}  // namespace

Lifted lift(const LiftInput& f) {
    Emitter e(f);
    Lifted out = e.run();
    if (out.ok) out.code = e.expand(out.code);
    return out;
}

LiftInput lift_input(const Cfg& cfg, const Function& f) {
    LiftInput in;
    in.start = f.start;
    for (uint32_t a : f.insns) {
        const Insn i = cfg.insn(a);
        in.insns.emplace_back(a, i);
        if (i.op == Op::b && i.lk() && i.branch_target(a) == a + 4) in.not_calls.insert(a);
    }
    in.jtabs = f.jtabs;
    in.falls_into = f.falls_into;
    const auto& fs = cfg.functions();
    in.is_function = [&fs](uint32_t a) { return fs.contains(a); };
    return in;
}

}  // namespace qilin
