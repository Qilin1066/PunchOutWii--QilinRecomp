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
#include <cstring>
#include <ctime>
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
                   kCancelThread = 0x8020891Cu, kYieldThread = 0x80208590u, kExitThread = 0x80208838u, kJoinThread = 0x80208AFCu,
                   kGetCurrentThread = 0x80208024u, kInitMutex = 0x802064A4u, kLockMutex = 0x802064DCu,
                   kUnlockMutex = 0x802065B8u, kInitCond = 0x802066ECu, kWaitCond = 0x802066F0u,
                   kSignalCond = 0x802067CCu, kInitMessageQueue = 0x80205A98u, kSendMessage = 0x80205AF8u,
                   kReceiveMessage = 0x80205BC0u, kJamMessage = 0x80205C9Cu, kCreateAlarm = 0x801FFCE4u,
                   kSetAlarm = 0x801FFF44u, kSleepTicks = 0x802092C8u, kGetTime = 0x80209370u,
                   kTicksToCalendarTime = 0x802095BCu, kShutdownSystem = 0x80206B84u, kRestart = 0x80206CA0u;

// Guest memory the tests use (MEM1, clear of the addresses the OS fixes).
constexpr uint32_t kThreadC = 0x80100800u, kStackC = 0x80128000u;
constexpr uint32_t kThreadA = 0x80100000u, kThreadB = 0x80100400u, kStackA = 0x80110000u, kStackB = 0x80120000u,
                   kStackSize = 0x8000u, kMutex = 0x80130000u, kCond = 0x80130100u, kQueue = 0x80130200u,
                   kQueueArray = 0x80130300u, kAlarm = 0x80130400u, kOut = 0x80130500u, kCalendar = 0x80130600u;

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
                   kFnSelf = 0x80004060u, kFnHolder = 0x80004070u, kFnWanter = 0x80004080u, kFnMiddle = 0x80004090u, kFnCancelled = 0x800040A0u, kFnOldSleep = 0x800040B0u, kFnNewSleep = 0x800040C0u, kFnChain = 0x800040D0u, kFnChainWaiter = 0x800040E0u;

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

// Priority inversion: a low thread holds a mutex a high one wants, and a middle one is ready.
void fn_holder(Cpu& c) {  // low
    os(c, kLockMutex, kMutex);
    note("low-locks");
    os(c, kResumeThread, kThreadB);  // the high one: runs at once, and blocks on the mutex
    note("low-goes-on");
    os(c, kResumeThread, kThreadC);  // the middle one: must not run before the mutex is free
    note("low-unlocks");
    os(c, kUnlockMutex, kMutex);
    note("low-done");
    c.r[3] = 0;
}
void fn_wanter(Cpu& c) {  // high
    note("high-wants");
    os(c, kLockMutex, kMutex);
    note("high-has-mutex");
    os(c, kUnlockMutex, kMutex);
    c.r[3] = 0;
}
void fn_middle(Cpu& c) {
    note("middle");
    c.r[3] = 0;
}

void fn_cancelled(Cpu& c) {
    os(c, kLockMutex, kMutex);
    note("cancel-owner");
    os(c, kSuspendThread, os(c, kGetCurrentThread));
    note("FAIL cancelled thread resumed");
}
void fn_old_sleep(Cpu& c) {
    const uint64_t ticks = punchout::kTimerHz / 1000;
    os(c, kSleepTicks, uint32_t(ticks >> 32), uint32_t(ticks));
    note("FAIL old sleeper resumed");
}

void fn_new_sleep(Cpu& c) {
    const uint64_t ticks = punchout::kTimerHz / 50; // 20 ms, later than the cancelled sleeper
    const auto start = std::chrono::steady_clock::now();
    os(c, kSleepTicks, uint32_t(ticks >> 32), uint32_t(ticks));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    note(elapsed >= std::chrono::milliseconds(10) ? "new-sleep-complete" : "FAIL new sleep woke at old deadline");
}

void fn_chain(Cpu& c) {
    os(c, kLockMutex, kMutex + 0x40);
    os(c, kLockMutex, kMutex);
    note("chain-runs");
    os(c, kUnlockMutex, kMutex);
    os(c, kUnlockMutex, kMutex + 0x40);
}
void fn_chain_waiter(Cpu& c) { os(c, kLockMutex, kMutex + 0x40); os(c, kUnlockMutex, kMutex + 0x40); }

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
    {kFnHolder, fn_holder},
    {kFnWanter, fn_wanter},
    {kFnMiddle, fn_middle},
    {kFnCancelled, fn_cancelled}, {kFnOldSleep, fn_old_sleep}, {kFnNewSleep, fn_new_sleep},
    {kFnChain, fn_chain}, {kFnChainWaiter, fn_chain_waiter},
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
    STUB(kShutdownSystem),
    STUB(kRestart),
    STUB(kGetCurrentThread),
    STUB(kYieldThread),
    STUB(kCreateThread),
    STUB(kExitThread),
    STUB(kCancelThread),
    STUB(kJoinThread),
    STUB(kResumeThread),
    STUB(kSuspendThread),
    STUB(kSleepTicks),
    STUB(kGetTime),
    STUB(kTicksToCalendarTime),
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

void test_priority_inheritance(Cpu& c) {
    os(c, kInitMutex, kMutex);
    create(c, kThreadA, kFnHolder, 0, kStackA, 20);
    create(c, kThreadB, kFnWanter, 0, kStackB, 8);
    create(c, kThreadC, kFnMiddle, 0, kStackC, 12);
    os(c, kResumeThread, kThreadA);  // below main: runs when main waits for it
    os(c, kJoinThread, kThreadA, 0);
    os(c, kJoinThread, kThreadB, 0);
    os(c, kJoinThread, kThreadC, 0);
    expect("a mutex's waiter lends its priority to the owner: no thread in between runs first",
           {"low-locks", "high-wants", "low-goes-on", "low-unlocks", "high-has-mutex", "middle", "low-done"});
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

void test_cancellation(Cpu& c) {
    // Cancellation must unlock held mutexes, and the native thread must keep
    // its object alive while it unwinds even if the guest address is reused.
    for (int i = 0; i < 64; ++i) {
        os(c, kInitMutex, kMutex);
        create(c, kThreadA, kFnCancelled, 0, kStackA, 8);
        os(c, kResumeThread, kThreadA);
        os(c, kCancelThread, kThreadA);
        if (rt::peek32(kMutex + 8) != 0 || rt::peek32(kMutex + 0xC) != 0)
            note("FAIL cancelled owner kept mutex");
        os(c, kLockMutex, kMutex);
        os(c, kUnlockMutex, kMutex);
        create(c, kThreadA, kFnHigh, 5, kStackA, 8);
        os(c, kResumeThread, kThreadA);
        os(c, kJoinThread, kThreadA, 0);
    }
    std::vector<std::string> expected;
    for (int i = 0; i < 64; ++i) { expected.push_back("cancel-owner"); expected.push_back("high 5"); }
    expect("cancelled owners release mutexes and thread addresses can be reused immediately", expected);

    // Cancel a timed sleeper, reuse its address for a self-suspended thread,
    // then let the old deadline pass. It must still need an explicit resume.
    create(c, kThreadA, kFnOldSleep, 0, kStackA, 8);
    os(c, kResumeThread, kThreadA);
    os(c, kCancelThread, kThreadA);
    create(c, kThreadA, kFnSelf, 0, kStackA, 8);
    os(c, kResumeThread, kThreadA);
    const uint64_t ticks = punchout::kTimerHz / 200;
    os(c, kSleepTicks, uint32_t(ticks >> 32), uint32_t(ticks));
    note("main-resumes-new");
    os(c, kResumeThread, kThreadA);
    os(c, kJoinThread, kThreadA, 0);
    expect("cancelled sleep deadline cannot resume a replacement thread",
           {"self-suspend", "main-resumes-new", "self-resumed"});
    create(c, kThreadA, kFnOldSleep, 0, kStackA, 8);
    os(c, kResumeThread, kThreadA);
    os(c, kCancelThread, kThreadA);
    create(c, kThreadA, kFnNewSleep, 0, kStackA, 8);
    os(c, kResumeThread, kThreadA);
    os(c, kJoinThread, kThreadA, 0);
    expect("a replacement sleeper keeps its own deadline", {"new-sleep-complete"});
}

void test_donation_withdrawal(Cpu& c) {
    os(c, kInitMutex, kMutex);
    os(c, kInitMutex, kMutex + 0x40);
    os(c, kLockMutex, kMutex);
    const uint32_t me = os(c, kGetCurrentThread);
    create(c, kThreadA, kFnChain, 0, kStackA, 12);
    os(c, kResumeThread, kThreadA);
    note("base-donation " + std::to_string(rt::peek32(me + 0x2D0)));
    create(c, kThreadB, kFnChainWaiter, 0, kStackB, 8);
    os(c, kResumeThread, kThreadB);
    note("chain-donation " + std::to_string(rt::peek32(me + 0x2D0)));
    os(c, kSuspendThread, kThreadB);
    note("suspended " + std::to_string(rt::peek32(me + 0x2D0)));
    os(c, kResumeThread, kThreadB);
    note("resumed " + std::to_string(rt::peek32(me + 0x2D0)));
    os(c, kCancelThread, kThreadB);
    note("cancelled " + std::to_string(rt::peek32(me + 0x2D0)));
    os(c, kCancelThread, kThreadA);
    note("restored " + std::to_string(rt::peek32(me + 0x2D0)));
    os(c, kUnlockMutex, kMutex);
    expect("priority donations propagate and withdraw across a chain",
           {"base-donation 12", "chain-donation 8", "suspended 12", "resumed 8", "cancelled 12", "restored 16"});
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

// OSTicksToCalendarTime and OSGetTime: dates from the time base, which
// counts from 1 January 2000 local time as on the console.
std::string calendar(Cpu& c, int64_t ticks) {
    c.r[3] = static_cast<uint32_t>(uint64_t(ticks) >> 32), c.r[4] = static_cast<uint32_t>(ticks), c.r[5] = kCalendar;
    rt::call(c, kTicksToCalendarTime);
    char out[96];
    const auto f = [](unsigned i) { return static_cast<int>(rt::peek32(kCalendar + 4 * i)); };
    std::snprintf(out, sizeof out, "%04d-%02d-%02d %02d:%02d:%02d.%03d%03d wday %d yday %d", f(5), f(4) + 1, f(3), f(2),
                  f(1), f(0), f(8), f(9), f(6), f(7));
    return out;
}

void test_calendar(Cpu& c) {
    const int64_t s = int64_t(punchout::kTimerHz), day = s * 86400;
    note(calendar(c, 0));
    note(calendar(c, -1 * s));  // a second before: 1999
    // 2024-02-29 (a leap day) 13:45:30.250128 (128 us is exactly 7776 ticks): 8825 days after 2000-01-01
    note(calendar(c, 8825 * day + (13 * 3600 + 45 * 60 + 30) * s + s / 4 + s * 128 / 1000000));
    note(calendar(c, 9819 * day));  // 2026-11-19
    expect("OSTicksToCalendarTime: dates, weekdays, leap days, before 2000",
           {"2000-01-01 00:00:00.000000 wday 6 yday 0", "1999-12-31 23:59:59.000000 wday 5 yday 364",
            "2024-02-29 13:45:30.250128 wday 4 yday 59", "2026-11-19 00:00:00.000000 wday 4 yday 322"});

    // The time base is the PC's local date and time.
    rt::call(c, kGetTime);
    const int64_t now = static_cast<int64_t>(uint64_t(c.r[3]) << 32 | c.r[4]);
    const std::string got = calendar(c, now).substr(0, 16);  // to the minute
    const std::time_t t = std::time(nullptr);
    char want[32];
    std::strftime(want, sizeof want, "%Y-%m-%d %H:%M", std::localtime(&t));
    note(got == want ? "today" : got + " but it's " + want);
    expect("OSGetTime is the local date and time", {"today"});
}

// The memory map as lifted code reaches it (cpu.hpp's loads and stores): the
// locked cache is plain memory of its own, the uncached mirrors are MEM1 and
// MEM2 again, and all three take any access size.
void test_memory_map(Cpu& c) {
    using namespace qilin::rt;
    char line[96];
    write32(c, 0xE0000010u, 0x11223344u);  // the locked cache
    write16(c, 0xE0003FFEu, 0xBEEFu);      // its last two bytes
    write8(c, 0xE0000014u, 0x5Au);
    std::snprintf(line, sizeof line, "%08X %04X %02X %02X", read32(c, 0xE0000010u), read16(c, 0xE0003FFEu), read8(c, 0xE0000014u),
                  read8(c, 0xE0000011u));
    note(line);
    write32(c, 0x80001000u, 0xCAFEF00Du);  // MEM1, read back through its uncached mirror
    write32(c, 0xC0001004u, 0x01020304u);  // and written through it
    std::snprintf(line, sizeof line, "%08X %08X %04X", read32(c, 0xC0001000u), read32(c, 0x80001004u), read16(c, 0xC0001006u));
    note(line);
    write32(c, 0x90002000u, 0xA1B2C3D4u);  // MEM2 and its mirror
    write8(c, 0xD0002003u, 0xEEu);
    std::snprintf(line, sizeof line, "%08X %08X", read32(c, 0xD0002000u), read32(c, 0x90002000u));
    note(line);
    expect("the locked cache and the uncached mirrors are plain memory",
           {"11223344 BEEF 5A 22", "CAFEF00D 01020304 0304", "A1B2C3EE A1B2C3EE"});
}

}  // namespace

int main(int argc, char** argv) {
    punchout::set_command_line(argc, argv);
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
    // The game's way out (OSShutdownSystem and the rest) closes the program;
    // its reset starts it again with the same command line. Each is a ctest of
    // its own (CMakeLists.txt): the first must exit cleanly, the second must
    // come back and pass.
    if (const char* mode = std::getenv("THREADS_TEST_LEAVE")) {
        if (std::strcmp(mode, "shutdown") == 0) rt::call(c, kShutdownSystem);
        if (std::strcmp(mode, "restart") == 0) {
#if defined(_WIN32)
            _putenv("THREADS_TEST_LEAVE=restarted");
#else
            setenv("THREADS_TEST_LEAVE", "restarted", 1);
#endif
            rt::call(c, kRestart);
        }
        if (std::strcmp(mode, "restarted") == 0) std::printf("ok   OSRestart started the program again\n");
    }
    test_preemption(c);
    test_message_queue(c);
    test_mutex_and_condition(c);
    test_priority_inheritance(c);
    test_yield_and_suspend(c);
    test_exit(c);
    test_cancellation(c);
    test_donation_withdrawal(c);
    test_alarm_and_sleep(c);
    test_calendar(c);
    test_memory_map(c);
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    std::fflush(nullptr);
    std::_Exit(g_failures ? 1 : 0);  // other guest threads are parked on host threads
}
