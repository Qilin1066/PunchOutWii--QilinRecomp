// The NAND library's replacements (revolution/nand): the Wii's flash file
// system, as a folder tree under this port's user folder (host/paths.hpp),
// <user folder>/nand/... with the guest's paths below it. The game's own data
// lives in its home directory, /title/00010000/52375050/data ("R7PP").
//
// Guest structures (RVL SDK, nand.h): a NANDFileInfo holds the file
// descriptor at +0 (here, an index into the open files); a NANDCommandBlock
// holds the user's data at +0 and, for an asynchronous call, its callback at
// +4. Callbacks are cb(result, block), run as an interrupt.
//
// The "safe" open and close work as on the Wii: writes go to a copy in
// /tmp, and the close swaps it in with one rename (after flushing it to the
// disk), so a crash or power cut mid-save leaves the old save whole. Copies
// left by such a stop are deleted at the next start.
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "boot.hpp"
#include "os.hpp"
#include "qilin/host/paths.hpp"

namespace punchout {
namespace rt = qilin::rt;
using rt::Cpu;
namespace fs = std::filesystem;

namespace {

// NAND_RESULT_* (nand.h).
constexpr int32_t kOk = 0, kAccess = -1, kExists = -6, kInvalid = -8, kMaxFd = -10, kNoExists = -12;
constexpr const char* kHomeDir = "/title/00010000/52375050/data";

fs::path g_root;
std::string g_current = kHomeDir;
std::vector<FILE*> g_files;  // by descriptor
std::vector<fs::path> g_safe_targets;  // by descriptor: for a safe open's copy, the file it replaces on close
constexpr const char* kSafeSuffix = ".safe";

// Flush a file to the disk itself (not just the OS's cache): before a rename
// makes it the save.
bool sync_file(FILE* f) {
    if (std::fflush(f) != 0) return false;
#if defined(_WIN32)
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

std::string guest_string(uint32_t a) { return reinterpret_cast<const char*>(rt::guest_ptr(a)); }

// A guest path, absolute or relative to the current directory, normalised:
// "." and ".." resolved, at most up to the root. Empty if it's malformed.
std::string absolute(const std::string& path) {
    std::vector<std::string> parts;
    auto push = [&](const std::string& p) {
        size_t i = 0;
        while (i <= p.size()) {
            const size_t j = std::min(p.find('/', i), p.size());
            const std::string part = p.substr(i, j - i);
            if (part == "..") {
                if (!parts.empty()) parts.pop_back();
            } else if (!part.empty() && part != ".") {
                parts.push_back(part);
            }
            i = j + 1;
        }
    };
    if (path.empty() || path[0] != '/') push(g_current);
    push(path);
    std::string out;
    for (const auto& p : parts) out += "/" + p;
    return out.empty() ? "/" : out;
}
fs::path host_path(const std::string& guest) { return g_root / absolute(guest).substr(1); }

void init_root() {
    if (!g_root.empty()) return;
    g_root = qilin::host::user_folder() / "nand";
    std::error_code ec;
    fs::create_directories(g_root / std::string(kHomeDir).substr(1), ec);
    fs::create_directories(g_root / "tmp", ec);
    if (ec) rt::fatal(nullptr, "can't create the save folder %s: %s", g_root.string().c_str(), ec.message().c_str());
    // A safe write cut short last time: its copy goes, the save it would have replaced stays.
    for (const auto& e : fs::directory_iterator(g_root / "tmp", ec))
        if (e.path().extension() == kSafeSuffix) {
            rt::log("saves: a save was cut short last time (%s): the one before it is kept", e.path().filename().string().c_str());
            fs::remove(e.path(), ec);
        }
    rt::log("saves: %s", g_root.string().c_str());
}

void reply(Cpu& c, int32_t result) { c.r[3] = static_cast<uint32_t>(result); }
// An asynchronous call: the result goes to cb(result, block), as an interrupt.
void reply_async(Cpu& c, int32_t result, uint32_t cb, uint32_t block) {
    rt::poke32(block + 4, cb);
    post_interrupt([result, cb, block](Cpu& cc) { guest_callback(cc, cb, static_cast<uint32_t>(result), block); });
    c.r[3] = static_cast<uint32_t>(kOk);
}

FILE* file_of(Cpu& c, uint32_t info) {
    const uint32_t fd = rt::peek32(info);
    if (fd >= g_files.size() || !g_files[fd]) rt::fatal(&c, "NAND file info %08X holds no open file", info);
    return g_files[fd];
}

// ---- the operations, each giving a NAND result
int32_t do_create(const std::string& path) {
    const fs::path p = host_path(path);
    std::error_code ec;
    if (fs::exists(p, ec)) return kExists;
    if (!fs::is_directory(p.parent_path(), ec)) return kNoExists;
    FILE* f = std::fopen(p.string().c_str(), "wb");
    if (!f) return kAccess;
    std::fclose(f);
    return kOk;
}
int32_t do_delete(const std::string& path) {
    const fs::path p = host_path(path);
    std::error_code ec;
    if (!fs::exists(p, ec)) return kNoExists;
    fs::remove_all(p, ec);
    return ec ? kAccess : kOk;
}
// mode: 1 read, 2 write, 3 both. `safe` (and writing): through a copy, swapped in on close.
int32_t do_open(const std::string& path, uint32_t info, uint32_t mode, bool safe = false) {
    const fs::path p = host_path(path);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return fs::exists(p, ec) ? kInvalid : kNoExists;
    size_t fd = 0;
    while (fd < g_files.size() && g_files[fd]) ++fd;
    if (fd >= 15) return kMaxFd;  // the Wii's limit on open files
    if (fd == g_files.size()) g_files.push_back(nullptr), g_safe_targets.emplace_back();
    safe = safe && mode != 1;
    fs::path open_path = p;
    if (safe) {
        open_path = g_root / "tmp" / (p.filename().string() + "." + std::to_string(fd) + kSafeSuffix);
        fs::copy_file(p, open_path, fs::copy_options::overwrite_existing, ec);
        if (ec) return kAccess;
    }
    FILE* f = std::fopen(open_path.string().c_str(), mode == 1 ? "rb" : "r+b");
    if (!f) return kAccess;
    g_files[fd] = f;
    g_safe_targets[fd] = safe ? p : fs::path{};
    rt::poke32(info, static_cast<uint32_t>(fd));
    return kOk;
}
int32_t do_close(Cpu& c, uint32_t info) {
    FILE* f = file_of(c, info);
    const uint32_t fd = rt::peek32(info);
    const fs::path target = std::move(g_safe_targets[fd]);
    g_safe_targets[fd].clear();
    g_files[fd] = nullptr;
    rt::poke32(info, 0xFFFFFFFFu);
    if (target.empty()) {
        std::fclose(f);
        return kOk;
    }
    // A safe write: on the disk first, then one rename makes it the save.
    const fs::path copy = g_root / "tmp" / (target.filename().string() + "." + std::to_string(fd) + kSafeSuffix);
    const bool synced = sync_file(f);
    std::fclose(f);
    std::error_code ec;
    if (synced) fs::rename(copy, target, ec);
    if (!synced || ec) {
        rt::log("saves: %s couldn't be written (%s): the one before it is kept", target.string().c_str(),
                synced ? ec.message().c_str() : "flushing it to the disk failed");
        fs::remove(copy, ec);
        return kAccess;
    }
    return kOk;
}
int32_t do_read(Cpu& c, uint32_t info, uint32_t buf, uint32_t len) {
    return static_cast<int32_t>(std::fread(rt::guest_ptr(buf), 1, len, file_of(c, info)));
}
int32_t do_write(Cpu& c, uint32_t info, uint32_t buf, uint32_t len) {
    FILE* f = file_of(c, info);
    const size_t n = std::fwrite(rt::guest_ptr(buf), 1, len, f);
    std::fflush(f);
    return n == len ? static_cast<int32_t>(n) : kAccess;
}
int32_t do_seek(Cpu& c, uint32_t info, int32_t offset, uint32_t whence) {  // 0 start, 1 current, 2 end
    FILE* f = file_of(c, info);
    if (whence > 2 || std::fseek(f, offset, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END) != 0) return kInvalid;
    return static_cast<int32_t>(std::ftell(f));
}
int32_t do_length(Cpu& c, uint32_t info, uint32_t out) {
    FILE* f = file_of(c, info);
    const long at = std::ftell(f);
    std::fseek(f, 0, SEEK_END);
    rt::poke32(out, static_cast<uint32_t>(std::ftell(f)));
    std::fseek(f, at, SEEK_SET);
    return kOk;
}
int32_t do_move(const std::string& path, const std::string& dir) {
    const fs::path from = host_path(path), to = host_path(dir) / from.filename();
    std::error_code ec;
    if (!fs::exists(from, ec) || !fs::is_directory(host_path(dir), ec)) return kNoExists;
    if (fs::exists(to, ec)) return kExists;
    fs::rename(from, to, ec);
    return ec ? kAccess : kOk;
}
int32_t do_change_dir(const std::string& path) {
    const std::string a = absolute(path);
    std::error_code ec;
    if (!fs::is_directory(g_root / a.substr(1), ec)) return kNoExists;
    g_current = a;
    return kOk;
}

// ---- the API
void NANDInit(Cpu& c) {
    init_root();
    reply(c, kOk);
}
void NANDCreate(Cpu& c) { reply(c, do_create(guest_string(c.r[3]))); }                       // (path, perm, attr)
void NANDCreateAsync(Cpu& c) { reply_async(c, do_create(guest_string(c.r[3])), c.r[6], c.r[7]); }  // (path, perm, attr, cb, block)
void NANDDelete(Cpu& c) { reply(c, do_delete(guest_string(c.r[3]))); }
void NANDDeleteAsync(Cpu& c) { reply_async(c, do_delete(guest_string(c.r[3])), c.r[4], c.r[5]); }  // (path, cb, block)
void NANDRead(Cpu& c) { reply(c, do_read(c, c.r[3], c.r[4], c.r[5])); }                           // (info, buf, len)
void NANDReadAsync(Cpu& c) { reply_async(c, do_read(c, c.r[3], c.r[4], c.r[5]), c.r[6], c.r[7]); }
void NANDWrite(Cpu& c) { reply(c, do_write(c, c.r[3], c.r[4], c.r[5])); }
void NANDWriteAsync(Cpu& c) { reply_async(c, do_write(c, c.r[3], c.r[4], c.r[5]), c.r[6], c.r[7]); }
void NANDSeek(Cpu& c) { reply(c, do_seek(c, c.r[3], static_cast<int32_t>(c.r[4]), c.r[5])); }       // (info, offset, whence)
void NANDSeekAsync(Cpu& c) { reply_async(c, do_seek(c, c.r[3], static_cast<int32_t>(c.r[4]), c.r[5]), c.r[6], c.r[7]); }
void NANDMove(Cpu& c) { reply(c, do_move(guest_string(c.r[3]), guest_string(c.r[4]))); }           // (path, destDir)
void NANDGetLength(Cpu& c) { reply(c, do_length(c, c.r[3], c.r[4])); }                             // (info, u32*)
void NANDGetLengthAsync(Cpu& c) { reply_async(c, do_length(c, c.r[3], c.r[4]), c.r[5], c.r[6]); }
void NANDSetUserData(Cpu& c) { rt::poke32(c.r[3], c.r[4]); }
void NANDGetUserData(Cpu& c) { c.r[3] = rt::peek32(c.r[3]); }
void NANDOpen(Cpu& c) { reply(c, do_open(guest_string(c.r[3]), c.r[4], c.r[5])); }                 // (path, info, mode)
void NANDOpenAsync(Cpu& c) { reply_async(c, do_open(guest_string(c.r[3]), c.r[4], c.r[5]), c.r[6], c.r[7]); }
void NANDClose(Cpu& c) { reply(c, do_close(c, c.r[3])); }
void NANDCloseAsync(Cpu& c) { reply_async(c, do_close(c, c.r[3]), c.r[4], c.r[5]); }               // (info, cb, block)
// (path, info, mode, buf, len, cb, block): see the note at the top on "safe".
void NANDPrivateSafeOpenAsync(Cpu& c) { reply_async(c, do_open(guest_string(c.r[3]), c.r[4], c.r[5], true), c.r[8], c.r[9]); }
void NANDSafeCloseAsync(Cpu& c) { reply_async(c, do_close(c, c.r[3]), c.r[4], c.r[5]); }
void NANDGetHomeDir(Cpu& c) {
    std::strcpy(reinterpret_cast<char*>(rt::guest_ptr(c.r[3])), kHomeDir);
    reply(c, kOk);
}
void NANDGetCurrentDir(Cpu& c) {
    std::strcpy(reinterpret_cast<char*>(rt::guest_ptr(c.r[3])), g_current.c_str());
    reply(c, kOk);
}
void NANDChangeDir(Cpu& c) { reply(c, do_change_dir(guest_string(c.r[3]))); }
void NANDChangeDirAsync(Cpu& c) { reply_async(c, do_change_dir(guest_string(c.r[3])), c.r[4], c.r[5]); }  // (path, cb, block)
void NANDGetType(Cpu& c) {  // (path, u8* type): 1 file, 2 directory
    const fs::path p = host_path(guest_string(c.r[3]));
    std::error_code ec;
    if (!fs::exists(p, ec)) return reply(c, kNoExists);
    rt::poke8(c.r[4], fs::is_directory(p, ec) ? 2 : 1);
    reply(c, kOk);
}
// (fsBlocks, inodes, u32* answer): whether that much more would fit. Always.
void NANDCheck(Cpu& c) {
    rt::poke32(c.r[5], 0);
    reply(c, kOk);
}
void NANDCheckAsync(Cpu& c) {
    rt::poke32(c.r[5], 0);
    reply_async(c, kOk, c.r[6], c.r[7]);
}

const rt::Provided kReplacements[] = {
    {0x8025D104u, "NANDInit", NANDInit},
    {0x8025AAECu, "NANDCreate", NANDCreate},
    {0x8025ABD4u, "NANDPrivateCreateAsync", NANDCreateAsync},
    {0x8025AC4Cu, "NANDCreateAsync", NANDCreateAsync},
    {0x8025ACC4u, "NANDDelete", NANDDelete},
    {0x8025AE0Cu, "NANDDeleteAsync", NANDDeleteAsync},
    {0x8025AFA4u, "NANDRead", NANDRead},
    {0x8025B00Cu, "NANDReadAsync", NANDReadAsync},
    {0x8025B084u, "NANDWrite", NANDWrite},
    {0x8025B0ECu, "NANDWriteAsync", NANDWriteAsync},
    {0x8025B164u, "NANDSeek", NANDSeek},
    {0x8025B204u, "NANDSeekAsync", NANDSeekAsync},
    {0x8025B600u, "NANDMove", NANDMove},
    {0x8025B664u, "NANDGetLength", NANDGetLength},
    {0x8025B760u, "NANDGetLengthAsync", NANDGetLengthAsync},
    {0x8025BBE4u, "NANDSetUserData", NANDSetUserData},
    {0x8025BBECu, "NANDGetUserData", NANDGetUserData},
    {0x8025BD08u, "NANDOpen", NANDOpen},
    {0x8025BD94u, "NANDPrivateOpen", NANDOpen},
    {0x8025BE20u, "NANDOpenAsync", NANDOpenAsync},
    {0x8025BF88u, "NANDClose", NANDClose},
    {0x8025BFF4u, "NANDCloseAsync", NANDCloseAsync},
    {0x8025C07Cu, "NANDPrivateSafeOpenAsync", NANDPrivateSafeOpenAsync},
    {0x8025C698u, "NANDSafeCloseAsync", NANDSafeCloseAsync},
    {0x8025D59Cu, "NANDChangeDir", NANDChangeDir},
    {0x8025D5E0u, "NANDChangeDirAsync", NANDChangeDirAsync},
    {0x8025D6A4u, "NANDGetCurrentDir", NANDGetCurrentDir},
    {0x8025D720u, "NANDGetHomeDir", NANDGetHomeDir},
    {0x8025DA84u, "NANDGetType", NANDGetType},
    {0x8025DD08u, "NANDCheck", NANDCheck},
    {0x8025DE10u, "NANDCheckAsync", NANDCheckAsync},
};

}  // namespace

std::span<const rt::Provided> nand_replacements() { return kReplacements; }

}  // namespace punchout
