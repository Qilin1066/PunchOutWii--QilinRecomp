// The thread model (punchout/src/threads.cpp, os.cpp) without the game.
//
// The real scheduler and interrupt layer run "guest" functions written here
// in C++, entered through a function table as lifted code would be. Each test
// is a small program in the OS's API, and the order of events it records is
// checked against what the Wii's OS does: priorities and preemption, blocking
// on message queues, mutexes and condition variables, suspend counts, joins
// and exit values, alarms and OSSleepTicks.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "audio.hpp"
#include "boot.hpp"
#include "os.hpp"
#include "threads.hpp"

namespace rt = qilin::rt;
using rt::Cpu;

namespace punchout {
void dsp_interrupt_enables(uint32_t) {}  // no DSP here
}  // namespace punchout

namespace {

// The OS functions, at their addresses in the supported build (threads.cpp).
constexpr uint32_t kCreateThread = 0x802085CCu, kResumeThread = 0x80208C3Cu, kSuspendThread = 0x80208ED4u,
                   kYieldThread = 0x80208590u, kExitThread = 0x80208838u, kJoinThread = 0x80208AFCu,
                   kGetCurrentThread = 0x80208024u, kInitMutex = 0x802064A4u, kLockMutex = 0x802064DCu,
                   kUnlockMutex = 0x802065B8u, kInitCond = 0x802066ECu, kWaitCond = 0x802066F0u,
                   kSignalCond = 0x802067CCu, kInitMessageQueue = 0x80205A98u, kSendMessage = 0x80205AF8u,
                   kReceiveMessage = 0x80205BC0u, kJamMessage = 0x80205C9Cu, kCreateAlarm = 0x801FFCE4u,
                   kSetAlarm = 0x801FFF44u, kSleepTicks = 0x802092C8u;

// Guest memory the tests use (MEM1, clear of the addresses the OS fixes).
constexpr uint32_t kThreadA = 0x80100000u, kThreadB = 0x80100400u, kStackA = 0x80110000u, kStackB = 0x80120000u,
                   kStackSize = 0x8000u, kMutex = 0x80130000u, kCond = 0x80130100u, kQueue = 0x80130200u,
                   kQueueArray = 0x80130300u, kAlarm = 0x80130400u, kOut = 0x80130500u;

std::vector<std::string> g_log;
int g_failures = 0;

void note(const std::string& s) {
    g_log.push_back(s);
    if (std::getenv("THREADS_TEST_VERBOSE")) std::printf("  .. %s\n", s.c_str());
}

uint32_t os(Cpu& c, uint32_t fn, uint32_t a = 0, uint32_t b = 0, uint32_t d = 0, uint32_t e = 0, uint32_t f = 0,
            uint32_t g = 0, uint32_t h = 0) {
    c.r[3] = a, c.r[4] = b, c.r[5] = d, c.r[6] = e, c.r[7] = f, c.r[8] = g, c.r[9] = h;
    rt::call(c, fn);
    return c.r[3];
}
void create(Cpu& c, uint32_t thread, uint32_t entry, uint32_t param, uint32_t stack, int priority, bool detached = false) {
    if (!os(c, kCreateThread, thread, entry, param, stack, kStackSize, static_cast<uint32_t>(priority), detached))
        rt::fatal(&c, "OSCreateThread failed");
}

void expect(const char* test, const std::vector<std::string>& want) {
    if (g_log == want) {
        std::printf("ok   %s\n", test);
    } else {
        ++g_failures;
        std::printf("FAIL %s\n  want:", test);
        for (const auto& s : want) std::printf(" %s", s.c_str());
        std::printf("\n  got: ");
        for (const auto& s : g_log) std::printf(" %s", s.c_str());
        std::printf("\n");
    }
    g_log.clear();
}

// ---- the guest functions, at made-up addresses in the function table.
constexpr uint32_t kFnHigh = 0x80004000u, kFnLowSender = 0x80004010u, kFnMutexTaker = 0x80004020u,
                   kFnCounter = 0x80004030u, kFnExiter = 0x80004040u, kFnAlarm = 0x80004050u,
                   kFnSelf = 0x80004060u;

void fn_high(Cpu& c) {  // runs as soon as it's resumed, and returns a value
    note("high " + std::to_string(c.r[3]));
    c.r[3] = 42;
}
void fn_low_sender(Cpu& c) {  // runs only when main blocks; main preempts it on the send
    note("low");
    os(c, kSendMessage, kQueue, 7, 1);
    note("low-done");
    c.r[3] = 0;
}
void fn_mutex_taker(Cpu& c) {  // blocks on main's mutex, then signals main's condition
    note("taker");
    os(c, kLockMutex, kMutex);
    note("taker-has-mutex");
    os(c, kSignalCond, kCond);
    os(c, kUnlockMutex, kMutex);
    note("taker-done");
    c.r[3] = 0;
}
void fn_counter(Cpu& c) {  // counts each time it runs, yielding between
    for (int i = 0; i < 3; ++i) {
        note("count " + std::to_string(i));
        os(c, kYieldThread);
    }
    c.r[3] = 0;
}
void fn_exiter(Cpu& c) {  // OSExitThread from deep inside, with a mutex held
    os(c, kLockMutex, kMutex);
    note("exiter");
    os(c, kExitThread, 99);
    note("not reached");
}
void fn_alarm(Cpu& c) {  // an alarm handler: (alarm, context)
    note("alarm");
    (void)c;
}
void fn_self(Cpu& c) {  // suspends itself; main resumes it
    note("self-suspend");
    os(c, kSuspendThread, os(c, kGetCurrentThread));
    note("self-resumed");
    c.r[3] = 0;
}

// What qilin lift generates for a replaced function: a stub in the function
// table that hands the call to the runtime's registry.
template <uint32_t A>
void replaced(Cpu& c) {
    c.hooks->replaced(c, A);
}
#define STUB(a) {a, replaced<a>}

const rt::FunctionEntry kFunctions[] = {  // sorted by address
    {kFnHigh, fn_high},
    {kFnLowSender, fn_low_sender},
    {kFnMutexTaker, fn_mutex_taker},
    {kFnCounter, fn_counter},
    {kFnExiter, fn_exiter},
    {kFnAlarm, fn_alarm},
    {kFnSelf, fn_self},
    STUB(kCreateAlarm),
    STUB(kSetAlarm),
    STUB(kInitMessageQueue),
    STUB(kSendMessage),
    STUB(kReceiveMessage),
    STUB(kJamMessage),
    STUB(kInitMutex),
    STUB(kLockMutex),
    STUB(kUnlockMutex),
    STUB(kInitCond),
    STUB(kWaitCond),
    STUB(kSignalCond),
    STUB(kGetCurrentThread),
    STUB(kYieldThread),
    STUB(kCreateThread),
    STUB(kExitThread),
    STUB(kJoinThread),
    STUB(kResumeThread),
    STUB(kSuspendThread),
    STUB(kSleepTicks),
};
#undef STUB

// ---- the tests, run in order by the main thread (priority 16).
void test_preemption(Cpu& c) {
    create(c, kThreadA, kFnHigh, 1, kStackA, 8);
    note("created");
    os(c, kResumeThread, kThreadA);  // outranks main: runs before this returns
    note("back");
    os(c, kJoinThread, kThreadA, kOut);
    note("joined " + std::to_string(rt::peek32(kOut)));
    expect("a higher priority thread runs as soon as it is resumed", {"created", "high 1", "back", "joined 42"});
}

void test_message_queue(Cpu& c) {
    os(c, kInitMessageQueue, kQueue, kQueueArray, 2);
    create(c, kThreadA, kFnLowSender, 0, kStackA, 20);
    os(c, kResumeThread, kThreadA);  // lower priority: main goes on
    note("main");
    os(c, kReceiveMessage, kQueue, kOut, 1);  // blocks: the sender runs; its send readies main, which preempts it
    note("got " + std::to_string(rt::peek32(kOut)));
    os(c, kJoinThread, kThreadA, 0);
    note("joined");
    expect("a blocking receive runs a lower thread, and the send preempts it", {"main", "low", "got 7", "low-done", "joined"});

    // FIFO order, a full queue refuses a non-blocking send, a jam goes first.
    os(c, kInitMessageQueue, kQueue, kQueueArray, 2);
    note(std::to_string(os(c, kSendMessage, kQueue, 1, 0)));
    note(std::to_string(os(c, kSendMessage, kQueue, 2, 0)));
    note(std::to_string(os(c, kSendMessage, kQueue, 3, 0)));  // full
    os(c, kReceiveMessage, kQueue, kOut, 0);
    note("r" + std::to_string(rt::peek32(kOut)));
    note(std::to_string(os(c, kJamMessage, kQueue, 9, 0)));
    for (int i = 0; i < 2; ++i) {
        os(c, kReceiveMessage, kQueue, kOut, 0);
        note("r" + std::to_string(rt::peek32(kOut)));
    }
    note(std::to_string(os(c, kReceiveMessage, kQueue, kOut, 0)));  // empty
    expect("message queues keep order, refuse when full, jam to the front",
           {"1", "1", "0", "r1", "1", "r9", "r2", "0"});
}

void test_mutex_and_condition(Cpu& c) {
    os(c, kInitMutex, kMutex);
    os(c, kInitCond, kCond);
    os(c, kLockMutex, kMutex);
    os(c, kLockMutex, kMutex);  // recursive
    create(c, kThreadA, kFnMutexTaker, 0, kStackA, 8);
    os(c, kResumeThread, kThreadA);  // runs, blocks on the mutex, main goes on
    os(c, kUnlockMutex, kMutex);     // still held once: nothing happens, and the wait restores this count
    note("main-waits");
    os(c, kWaitCond, kCond, kMutex);  // releases it entirely; the taker runs to the end
    note("main-woke owner=" + std::to_string(rt::peek32(kMutex + 8) == os(c, kGetCurrentThread)) +
         " count=" + std::to_string(rt::peek32(kMutex + 0xC)));
    os(c, kUnlockMutex, kMutex);
    os(c, kJoinThread, kThreadA, 0);
    expect("a mutex blocks its taker; waiting on a condition releases it, recursion included",
           {"taker", "main-waits", "taker-has-mutex", "taker-done", "main-woke owner=1 count=1"});
}

void test_yield_and_suspend(Cpu& c) {
    // Yielding passes to threads of the same priority in turn, never to lower ones.
    create(c, kThreadA, kFnCounter, 0, kStackA, 16);
    create(c, kThreadB, kFnHigh, 2, kStackB, 30);
    os(c, kResumeThread, kThreadA);
    os(c, kResumeThread, kThreadB);
    for (int i = 0; i < 2; ++i) {
        note("main " + std::to_string(i));
        os(c, kYieldThread);
    }
    os(c, kJoinThread, kThreadA, 0);  // blocks: the counter finishes, then the low one runs
    os(c, kJoinThread, kThreadB, 0);
    expect("yield takes turns within a priority", {"main 0", "count 0", "main 1", "count 1", "count 2", "high 2"});

    // Suspend counts: a created thread starts at 1; two suspends need two more resumes.
    create(c, kThreadA, kFnHigh, 3, kStackA, 8);
    note(std::to_string(os(c, kSuspendThread, kThreadA)));
    note(std::to_string(os(c, kResumeThread, kThreadA)));
    note(std::to_string(os(c, kResumeThread, kThreadA)));  // 1 -> 0: runs before the call returns
    os(c, kJoinThread, kThreadA, 0);
    // A thread suspending itself runs again only once resumed.
    create(c, kThreadA, kFnSelf, 0, kStackA, 8);
    os(c, kResumeThread, kThreadA);
    note("main");
    os(c, kResumeThread, kThreadA);
    os(c, kJoinThread, kThreadA, 0);
    expect("suspend counts", {"1", "2", "high 3", "1", "self-suspend", "main", "self-resumed"});
}

void test_exit(Cpu& c) {
    os(c, kInitMutex, kMutex);
    create(c, kThreadA, kFnExiter, 0, kStackA, 8);
    os(c, kResumeThread, kThreadA);
    note("owner " + std::to_string(rt::peek32(kMutex + 8)));  // released by the exit
    const uint32_t joined = os(c, kJoinThread, kThreadA, kOut);  // before reading what it wrote
    note("join " + std::to_string(joined) + " " + std::to_string(rt::peek32(kOut)));
    create(c, kThreadA, kFnHigh, 4, kStackA, 8, true);  // detached: can't be joined
    os(c, kResumeThread, kThreadA);
    note("join " + std::to_string(os(c, kJoinThread, kThreadA, kOut)));
    expect("OSExitThread ends a thread with its value and releases its mutexes",
           {"exiter", "owner 0", "join 1 99", "high 4", "join 0"});
}

void test_alarm_and_sleep(Cpu& c) {
    // An alarm in 1 ms; main sleeps 5 ms with nothing else to run: the alarm's
    // handler runs while every thread waits, then main wakes on time.
    const uint64_t ms = punchout::kTimerHz / 1000;
    const auto t0 = std::chrono::steady_clock::now();
    os(c, kCreateAlarm, kAlarm);
    c.r[3] = kAlarm, c.r[4] = 0, c.r[5] = 0, c.r[6] = static_cast<uint32_t>(ms), c.r[7] = kFnAlarm;
    rt::call(c, kSetAlarm);
    c.r[3] = 0, c.r[4] = static_cast<uint32_t>(5 * ms);
    rt::call(c, kSleepTicks);
    const auto took = std::chrono::steady_clock::now() - t0;
    note("slept");
    note(took >= std::chrono::milliseconds(5) ? "on time" : "early");
    note(rt::peek32(kAlarm) == 0 ? "disarmed" : "armed");  // a one-shot alarm clears its handler
    expect("an alarm fires while every thread waits, and OSSleepTicks wakes on time",
           {"alarm", "slept", "on time", "disarmed"});
}

}  // namespace

int main() {
    // Every OS function the tests call is a replacement in the real build.
    static std::vector<rt::ReplacedEntry> replaced;
    for (auto list : {punchout::thread_replacements(), punchout::os_replacements()})
        for (const auto& p : list) replaced.push_back({p.address, "test", false});
    std::ranges::sort(replaced, {}, &rt::ReplacedEntry::address);
    rt::init({kFunctions, replaced});
    rt::provide(punchout::thread_replacements());
    rt::provide(punchout::os_replacements());

    static Cpu main_cpu = rt::new_cpu();
    main_cpu.r[1] = 0x80180000u;
    punchout::init_threads(main_cpu);
    punchout::set_interrupts_enabled(true);

    Cpu& c = main_cpu;
    test_preemption(c);
    test_message_queue(c);
    test_mutex_and_condition(c);
    test_yield_and_suspend(c);
    test_exit(c);
    test_alarm_and_sleep(c);
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    std::fflush(nullptr);
    std::_Exit(g_failures ? 1 : 0);  // other guest threads are parked on host threads
}
