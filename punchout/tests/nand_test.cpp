// Real NAND replacements over synthetic guest memory and a temporary save tree.
#include "boot.hpp"
#include "os.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>
#include <chrono>
#if !defined(_WIN32)
#include <sys/resource.h>
#include <csignal>
#endif

namespace rt = qilin::rt;
namespace fs = std::filesystem;
namespace {
fs::path folder;
std::vector<std::function<void(rt::Cpu&)>> pending;
unsigned callbacks = 0;
uint32_t last_callback_result = 0;
}
namespace qilin::host {
fs::path user_folder() { return folder; }
}
namespace punchout {
void post_interrupt(std::function<void(rt::Cpu&)> fn) { pending.push_back(std::move(fn)); }
void guest_callback(rt::Cpu&, uint32_t, uint32_t result, uint32_t) { ++callbacks; last_callback_result = result; }
}
int main() {
    folder = fs::temp_directory_path() / ("punchout-nand-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    rt::init({{}, {}});
    auto cpu = rt::new_cpu();
    constexpr uint32_t path = 0x80100000, info = path + 0x100, buffer = path + 0x200, result = path + 0x300, block = path + 0x400;
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++failures;
    };
    const auto call = [&](const char* name, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0,
                          uint32_t e = 0, uint32_t f = 0, uint32_t g = 0) {
        cpu.r[3] = a; cpu.r[4] = b; cpu.r[5] = c; cpu.r[6] = d; cpu.r[7] = e; cpu.r[8] = f; cpu.r[9] = g;
        for (const auto& replacement : punchout::nand_replacements())
            if (std::strcmp(name, replacement.name) == 0) { replacement.fn(cpu); return int32_t(cpu.r[3]); }
        std::fprintf(stderr, "unknown test replacement: %s\n", name);
        ++failures;
        return int32_t(-999);
    };
    check(call("NANDInit") == 0, "NAND starts in temporary tree");
    std::strcpy(reinterpret_cast<char*>(rt::guest_ptr(path)), "save.dat");
    check(call("NANDCreate", path) == 0 && call("NANDCreate", path) == -6, "create and duplicate");
    check(call("NANDOpen", path, info, 0) == -8 && call("NANDOpen", path, info, 4) == -8, "invalid open modes rejected");
    check(call("NANDOpen", path, info, 3) == 0, "read-write open");
    std::memcpy(rt::guest_ptr(buffer), "old-save", 8);
    check(call("NANDWrite", info, buffer, 8) == 8, "write original save");
    check(call("NANDSeek", info, 3, 0) == 3 && call("NANDGetLength", info, result) == 0 && rt::peek32(result) == 8,
          "length query reads correct length");
    check(call("NANDSeek", info, 0, 1) == 3, "length query preserves position");
    check(call("NANDSeek", info, 0, 7) == -8, "invalid seek origin rejected");
    check(call("NANDClose", info) == 0 && rt::peek32(info) == 0xFFFFFFFFu, "close invalidates descriptor");
    check(call("NANDOpen", path, info, 1) == 0, "read-only open");
    check(call("NANDWrite", info, buffer, 8) == -1, "failed host write reports access error");
    check(call("NANDClose", info) == -1, "failed write remains an error at close");
    const auto saved = folder / "nand/title/00010000/52375050/data/save.dat";
    const auto content = [&] { std::ifstream file(saved, std::ios::binary); return std::string(std::istreambuf_iterator<char>(file), {}); };
    check(content() == "old-save", "failed write leaves save intact");
    check(call("NANDPrivateSafeOpenAsync", path, info, 3, buffer, 32, 123, block) == 0 && callbacks == 0 && pending.size() == 1,
          "safe open queues rather than invokes callback");
    for (auto& fn : pending) fn(cpu);
    pending.clear();
    std::memcpy(rt::guest_ptr(buffer), "new-save", 8);
    check(call("NANDWrite", info, buffer, 8) == 8 && content() == "old-save", "safe write keeps old save until close");
    check(call("NANDSafeCloseAsync", info, 123, block) == 0 && content() == "new-save", "safe close commits complete replacement");
    check(fs::is_empty(folder / "nand/tmp"), "successful safe save leaves no temporary copies");
    check(call("NANDOpen", path, info, 1) == 0 && call("NANDRead", info, buffer, 32) == 8 &&
          std::memcmp(rt::guest_ptr(buffer), "new-save", 8) == 0, "short read at EOF returns bytes, not an error");
    call("NANDClose", info);
#if !defined(_WIN32)
    // Simulate a full/limited filesystem after a safe copy has been opened.
    // A later successful close must not commit a partially written copy.
    check(call("NANDPrivateSafeOpenAsync", path, info, 3, buffer, 32, 123, block) == 0, "safe open before injected write error");
    struct rlimit previous{};
    if (getrlimit(RLIMIT_FSIZE, &previous) == 0) {
        auto limited = previous; limited.rlim_cur = 8;
        const auto signal_before = std::signal(SIGXFSZ, SIG_IGN);
        if (setrlimit(RLIMIT_FSIZE, &limited) == 0) {
            std::memset(rt::guest_ptr(buffer), 'x', 16);
            const auto result_write = call("NANDWrite", info, buffer, 16);
            setrlimit(RLIMIT_FSIZE, &previous);
            check(result_write == -1, "failed flush reports write failure");
            check(call("NANDSafeCloseAsync", info, 123, block) == 0, "async close queues failed result");
            check(content() == "new-save", "failed safe write never replaces last complete save");
            for (auto& fn : pending) fn(cpu);
            pending.clear();
            check(int32_t(last_callback_result) == -1, "failed safe close reaches its callback");
        } else { call("NANDClose", info); }
        std::signal(SIGXFSZ, signal_before);
    } else { call("NANDClose", info); }
#endif
    // A history-storage failure must preserve the last complete save too.
    check(call("NANDPrivateSafeOpenAsync", path, info, 3, buffer, 32, 123, block) == 0, "safe open before blocked backup");
    std::memcpy(rt::guest_ptr(buffer), "bad-save", 8);
    check(call("NANDWrite", info, buffer, 8) == 8, "replacement staged before history failure");
    fs::rename(folder / "save-backups", folder / "saved-history");
    { std::ofstream obstruction(folder / "save-backups"); obstruction << "blocked"; }
    call("NANDSafeCloseAsync", info, 123, block);
    for (auto& fn : pending) fn(cpu);
    pending.clear();
    check(int32_t(last_callback_result) == -1 && content() == "new-save", "backup failure aborts save replacement");
    fs::remove(folder / "save-backups"); fs::rename(folder / "saved-history", folder / "save-backups");
    // Descriptor limit, followed by reuse of a released slot.
    for (uint32_t i = 0; i < 15; ++i) check(call("NANDOpen", path, info + i * 4, 1) == 0, "open within descriptor limit");
    check(call("NANDOpen", path, info + 60, 1) == -10, "sixteenth open reports descriptor limit");
    for (uint32_t i = 0; i < 15; ++i) call("NANDClose", info + i * 4);
    check(call("NANDOpen", path, info, 1) == 0, "released descriptor is reusable");
    call("NANDClose", info);
    fs::remove_all(folder);
    return failures ? 1 : 0;
}
