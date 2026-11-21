// The DVD library's replacements: the disc's file system from sys/fst.bin,
// file data from the extracted files/ folder.
//
// Guest structures (RVL SDK, dvd.h): a DVDCommandBlock is 0x30 bytes, with its
// state at +0x0C (0 done, 1 busy) and the bytes transferred at +0x20; a
// DVDFileInfo is a command block, then the file's start (in 4-byte units, as
// the Wii's FST stores it) at +0x30, its length at +0x34 and the callback for
// asynchronous reads at +0x38.
#include <cstdlib>
#include <set>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "boot.hpp"
#include "os.hpp"

namespace punchout {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

struct Entry {
    bool dir;
    std::string name;
    uint32_t parent;  // dirs: the parent's index; files: the containing dir
    uint32_t start;   // files: offset on the disc / 4; dirs: unused
    uint32_t size;    // files: length; dirs: index after the last entry inside
    std::string path; // relative to files/, with '/'
};

std::vector<Entry> g_fst;
std::map<uint32_t, uint32_t> g_by_start;  // a file's start -> its entry
uint32_t g_current_dir = 0;
bool g_auto_invalidation = true;

uint32_t be32(const std::vector<uint8_t>& d, size_t at) {
    return uint32_t{d[at]} << 24 | uint32_t{d[at + 1]} << 16 | uint32_t{d[at + 2]} << 8 | d[at + 3];
}

void load_fst() {
    if (!g_fst.empty()) return;
    const auto path = game_folder() / "sys" / "fst.bin";
    std::vector<uint8_t> d;
    if (FILE* f = std::fopen(path.string().c_str(), "rb")) {
        uint8_t buf[65536];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
        std::fclose(f);
    }
    if (d.size() < 12) rt::fatal(nullptr, "cannot read the disc's file table %s", path.string().c_str());
    const uint32_t count = be32(d, 8);
    const size_t strings = size_t{count} * 12;
    if (count == 0 || strings > d.size()) rt::fatal(nullptr, "%s is malformed", path.string().c_str());
    g_fst.resize(count);
    // Directories own the entries up to their end index; walk with a stack of them.
    std::vector<uint32_t> dirs{0};
    g_fst[0] = {true, "", 0, 0, count, ""};
    for (uint32_t i = 1; i < count; ++i) {
        while (dirs.size() > 1 && i >= g_fst[dirs.back()].size) dirs.pop_back();
        const size_t e = size_t{i} * 12;
        const uint32_t name_at = be32(d, e) & 0xFFFFFFu;
        std::string name;
        for (size_t k = strings + name_at; k < d.size() && d[k]; ++k) name += static_cast<char>(d[k]);
        Entry& x = g_fst[i];
        x.dir = d[e] != 0;
        x.name = name;
        x.parent = dirs.back();
        x.start = be32(d, e + 4);
        x.size = be32(d, e + 8);
        x.path = g_fst[x.parent].path.empty() ? name : g_fst[x.parent].path + "/" + name;
        if (x.dir) {
            x.parent = be32(d, e + 4);
            dirs.push_back(i);
        } else {
            g_by_start[x.start] = i;
        }
    }
}

bool same_name(const std::string& a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// The SDK's lookup: from the current directory, or the root after a leading
// '/'; "." and ".." step in place and up; names compare without case.
int32_t path_to_entry(std::string_view path) {
    uint32_t dir = g_current_dir;
    if (!path.empty() && path[0] == '/') {
        dir = 0;
        path.remove_prefix(1);
    }
    while (!path.empty()) {
        const size_t slash = path.find('/');
        const std::string_view part = path.substr(0, slash);
        path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            dir = g_fst[dir].parent;
            continue;
        }
        bool found = false;
        for (uint32_t i = dir + 1; i < g_fst[dir].size;) {
            if (same_name(g_fst[i].name, part)) {
                if (!g_fst[i].dir && !path.empty()) return -1;  // a file used as a directory
                dir = i;
                found = true;
                break;
            }
            i = g_fst[i].dir ? g_fst[i].size : i + 1;
        }
        if (!found) return -1;
    }
    return static_cast<int32_t>(dir);
}

// Read from the file whose FST start is `start`: what the drive would return.
int32_t read_file(Cpu& c, uint32_t start, uint32_t addr, uint32_t length, uint32_t offset) {
    const auto it = g_by_start.find(start);
    if (it == g_by_start.end()) rt::fatal(&c, "DVD read from %08X, which starts no file", start << 2);
    const Entry& e = g_fst[it->second];
    if (uint64_t{offset} + length > ((uint64_t{e.size} + 31) & ~uint64_t{31}))
        rt::fatal(&c, "DVD read past the end of %s (%u bytes at %u)", e.path.c_str(), length, offset);
    const auto host = game_folder() / "files" / e.path;
    FILE* f = std::fopen(host.string().c_str(), "rb");
    if (!f) {
        // Development, with part of a disc: PUNCHOUT_DEV_MISSING_FILES=<list> reads a missing file
        // as zeros and adds its name to the list (once), to find what a scene needs.
        if (const char* list = std::getenv("PUNCHOUT_DEV_MISSING_FILES")) {
            static std::set<std::string> said;
            if (said.insert(e.path).second)
                if (FILE* out = std::fopen(list, "a")) {
                    std::fprintf(out, "%s\n", e.path.c_str());
                    std::fclose(out);
                }
            std::memset(rt::guest_ptr(addr), 0, length);
            return static_cast<int32_t>(length);
        }
        rt::fatal(&c, "the game reads %s, which isn't there: extract the whole disc (files/ next to sys/)",
                  host.string().c_str());
    }
    std::memset(rt::guest_ptr(addr), 0, length);  // reads past the end of the file, up to the 32-byte unit, give zeros
    std::fseek(f, static_cast<long>(offset), SEEK_SET);
    const size_t got = std::fread(rt::guest_ptr(addr), 1, length, f);
    std::fclose(f);
    if (got < length && offset + got < e.size) rt::fatal(&c, "short read from %s", host.string().c_str());
    return static_cast<int32_t>(length);
}

void finish(uint32_t block, int32_t result) {
    rt::poke32(block + 0x20, result < 0 ? 0 : static_cast<uint32_t>(result));
    rt::poke32(block + 0x0C, result < 0 ? 0xFFFFFFFFu : 0);  // DVD_STATE_END, or FATAL_ERROR
}

// ---- the API
void DVDInit(Cpu&) { load_fst(); }

void DVDConvertPathToEntrynum(Cpu& c) {
    load_fst();
    c.r[3] = static_cast<uint32_t>(path_to_entry(reinterpret_cast<const char*>(rt::guest_ptr(c.r[3]))));
}

void DVDFastOpen(Cpu& c) {  // (entrynum, fileInfo) -> TRUE, or FALSE for a directory or a bad number
    load_fst();
    const uint32_t n = c.r[3], fi = c.r[4];
    if (n == 0 || n >= g_fst.size() || g_fst[n].dir) {
        c.r[3] = 0;
        return;
    }
    rt::poke32(fi + 0x30, g_fst[n].start);
    rt::poke32(fi + 0x34, g_fst[n].size);
    rt::poke32(fi + 0x38, 0);
    rt::poke32(fi + 0x0C, 0);  // DVD_STATE_END
    c.r[3] = 1;
}

void DVDClose(Cpu& c) { c.r[3] = 1; }  // nothing is ever left in flight

void DVDReadPrio(Cpu& c) {  // (fileInfo, addr, length, offset, prio) -> bytes read
    const uint32_t fi = c.r[3];
    const int32_t n = read_file(c, rt::peek32(fi + 0x30), c.r[4], c.r[5], c.r[6]);
    finish(fi, n);
    c.r[3] = static_cast<uint32_t>(n);
}

void DVDReadAsyncPrio(Cpu& c) {  // (fileInfo, addr, length, offset, callback, prio) -> TRUE
    const uint32_t fi = c.r[3], callback = c.r[7];
    const int32_t n = read_file(c, rt::peek32(fi + 0x30), c.r[4], c.r[5], c.r[6]);
    rt::poke32(fi + 0x38, callback);
    rt::poke32(fi + 0x0C, 1);  // busy until the "interrupt" says it's done
    post_interrupt([fi, n, callback](Cpu& cc) {
        finish(fi, n);
        if (callback) guest_callback(cc, callback, static_cast<uint32_t>(n), fi);
    });
    c.r[3] = 1;
}

void DVDGetCommandBlockStatus(Cpu& c) {
    deliver_interrupts(c);  // a polling loop: let a pending completion arrive
    const uint32_t s = rt::peek32(c.r[3] + 0x0C);
    c.r[3] = s == 3 ? 1 : s;  // DVD_STATE_COVER_CLOSED reads as busy
}

void DVDGetDriveStatus(Cpu& c) {
    deliver_interrupts(c);
    c.r[3] = 0;  // idle (nothing stays in flight past a safe point with interrupts on)
}

void DVDSetAutoInvalidation(Cpu& c) {
    const bool was = g_auto_invalidation;
    g_auto_invalidation = c.r[3] != 0;
    c.r[3] = was;
}

const rt::Provided kReplacements[] = {
    {0x801E2AD4u, "DVDInit", DVDInit},
    {0x801E2470u, "DVDConvertPathToEntrynum", DVDConvertPathToEntrynum},
    {0x801E2778u, "DVDFastOpen", DVDFastOpen},
    {0x801E27E0u, "DVDClose", DVDClose},
    {0x801E28ECu, "DVDReadPrio", DVDReadPrio},
    {0x801E2804u, "DVDReadAsyncPrio", DVDReadAsyncPrio},
    {0x801E6B40u, "DVDGetCommandBlockStatus", DVDGetCommandBlockStatus},
    {0x801E6C08u, "DVDGetDriveStatus", DVDGetDriveStatus},
    {0x801E6CA4u, "DVDSetAutoInvalidation", DVDSetAutoInvalidation},
};

}  // namespace

std::span<const rt::Provided> dvd_replacements() { return kReplacements; }

}  // namespace punchout
