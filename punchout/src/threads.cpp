// Threads, mutexes, condition variables, message queues and alarms
// (revolution/os's OSThread, OSMutex, OSMessage, OSAlarm), natively.
//
// Each guest thread runs on a host thread of its own, with its own registers
// (a Cpu), and exactly one of them runs at a time: the scheduler hands the
// guest CPU over where the guest OS would switch, namely when the running thread
// blocks or yields, or when it wakes a thread of higher priority. Interrupts
// arrive at safe points (os.hpp), so preemption by an interrupt's wake-up
// happens there too. The rules are the OS's: priorities 0 (highest) to 31,
// first come first served within one, suspend counts, threads created
// suspended.
//
// Guest structures stay up to date where the game can read them: an OSThread's
// state, suspend count, priority and exit value, the current thread at
// 0x800000E4, a mutex's owner and count, a message queue's ring. Who waits on
// what is kept here, by the guest address of the queue waited on.
#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "boot.hpp"
#include "os.hpp"
#include "threads.hpp"

namespace punchout {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

// OSThread (0x318 bytes): its OSContext, then these.
constexpr uint32_t kState = 0x2C8, kAttr = 0x2CA, kSuspend = 0x2CC, kPriority = 0x2D0, kBase = 0x2D4, kVal = 0x2D8,
                   kQueue = 0x2DC, kQueueJoin = 0x2E8, kStackBase = 0x304, kStackEnd = 0x308;
enum : uint16_t { kReady = 1, kRunning = 2, kWaiting = 4, kMoribund = 8 };
constexpr uint32_t kCurrentThread = 0x800000E4u, kDefaultThreadPtr = 0x800000D8u;
constexpr uint32_t kActiveHead = 0x800000DCu, kActiveTail = 0x800000E0u;
constexpr uint32_t kDefaultThread = 0x803CB098u;     // __OSThreadInit's DefaultThread
constexpr uint32_t kMainStackEnd = 0x804192E4u;      // _stack_end
constexpr uint32_t kOSExitThread = 0x80208838u;      // a new thread's return address
constexpr uint32_t kFpuContextFlag = 0x803CAD30u;    // OSCreateThread fills FPRs with -1 when set
constexpr size_t kHostStack = 16u << 20;

struct ThreadExit {
    uint32_t value;
};

struct Thread {
    uint32_t guest = 0;
    Cpu* cpu = nullptr;          // the main thread's lives in start_game
    std::unique_ptr<Cpu> own;    // other threads' here
    int priority = 16;
    int suspend = 0;
    uint16_t state = kReady;
    bool detached = false;
    bool enabled = true;         // its interrupt-enable flag while it isn't running
    uint32_t waiting_on = 0;     // the queue it sleeps on, or 0
    uint32_t entry = 0;
    bool started = false, go = false;
    std::condition_variable cv;
    std::vector<uint32_t> mutexes;  // held, for OSExitThread's unlock
};

std::mutex g_lock;  // guards the hand-over (go flags, condition variables)
std::map<uint32_t, std::unique_ptr<Thread>> g_threads;
Thread* g_current = nullptr;
std::deque<Thread*> g_ready[32];
std::map<uint32_t, std::deque<Thread*>> g_waiters;

void store(Thread* t) {
    rt::poke16(t->guest + kState, t->state);
    rt::poke32(t->guest + kSuspend, static_cast<uint32_t>(t->suspend));
    rt::poke32(t->guest + kPriority, static_cast<uint32_t>(t->priority));
    rt::poke32(t->guest + kQueue, t->waiting_on);
}

Thread* thread_at(Cpu& c, uint32_t guest) {
    const auto it = g_threads.find(guest);
    if (it == g_threads.end()) rt::fatal(&c, "%08X isn't a thread OSCreateThread made", guest);
    return it->second.get();
}

void make_ready(Thread* t) {
    t->state = kReady;
    t->waiting_on = 0;
    if (t->suspend == 0) g_ready[t->priority].push_back(t);
    store(t);
}
void unready(Thread* t) {
    auto& q = g_ready[t->priority];
    for (auto it = q.begin(); it != q.end(); ++it)
        if (*it == t) {
            q.erase(it);
            return;
        }
}
Thread* best_ready() {
    for (auto& q : g_ready)
        if (!q.empty()) return q.front();
    return nullptr;
}

void host_main(Thread* t);

void start_host(Thread* t) {
    t->started = true;
    run_on_host_thread([](void* p) { host_main(static_cast<Thread*>(p)); }, t);
}

// Hand the guest CPU to `next`; unless `leaving`, wait until it comes back.
void switch_to(Thread* next, bool leaving = false) {
    Thread* me = g_current;
    me->enabled = interrupts_enabled();
    unready(next);
    next->state = kRunning;
    store(next);
    g_current = next;
    rt::poke32(kCurrentThread, next->guest);
    if (!next->started) start_host(next);
    {
        std::lock_guard l(g_lock);
        next->go = true;
    }
    next->cv.notify_one();
    if (leaving) return;
    std::unique_lock l(g_lock);
    me->cv.wait(l, [me] { return me->go; });
    me->go = false;
    l.unlock();
    set_interrupts_enabled(me->enabled);
}

// The running thread can't go on (it waits, is suspended, or has exited): run
// another, or, with none ready, act as the OS's idle loop until an interrupt
// readies one. Returns when this thread runs again (never if `leaving`).
void block(Cpu& c, bool leaving = false) {
    Thread* me = g_current;
    for (;;) {
        if (Thread* next = best_ready()) {
            if (next == me) {  // woken while still on its own host thread
                unready(me);
                me->state = kRunning;
                store(me);
                return;
            }
            switch_to(next, leaving);
            return;
        }
        if (deliver_interrupts_idle(c)) continue;
        const uint64_t deadline = next_timer_deadline();
        if (deadline == UINT64_MAX && !has_pending_interrupts())
            rt::fatal(&c, "every guest thread is waiting, and nothing is left to wake one (deadlock)");
        const uint64_t now = time_base();
        if (deadline > now)
            std::this_thread::sleep_for(std::chrono::nanoseconds((deadline - now) * 1000000000 / kTimerHz));
    }
}

void host_main(Thread* t) {
    {
        std::unique_lock l(g_lock);
        t->cv.wait(l, [t] { return t->go; });
        t->go = false;
    }
    set_interrupts_enabled(true);  // a new thread's context has interrupts on
    uint32_t value;
    try {
        rt::call(*t->cpu, t->entry);
        value = t->cpu->r[3];
    } catch (const ThreadExit& e) {
        value = e.value;
    }
    finish_thread(*t->cpu, value);
}

// Ready every thread sleeping on `queue`, without switching. True if any.
bool wake_waiters(uint32_t queue) {
    const auto it = g_waiters.find(queue);
    if (it == g_waiters.end() || it->second.empty()) return false;
    std::deque<Thread*> woken = std::move(it->second);
    g_waiters.erase(it);
    for (Thread* t : woken)
        if (t->state == kWaiting) make_ready(t);
    return true;
}

void sleep_on(Cpu& c, uint32_t queue) {
    Thread* me = g_current;
    me->state = kWaiting;
    me->waiting_on = queue;
    store(me);
    g_waiters[queue].push_back(me);
    block(c);
}

// Switch to a thread readied by the running one, if it outranks it.
void reschedule(Cpu& c) {
    if (in_interrupt()) return;  // the interrupt's end switches (preempt)
    Thread* me = g_current;
    Thread* best = best_ready();
    if (best && best->priority < me->priority) {
        make_ready(me);
        switch_to(best);
    }
    (void)c;
}

// ---- alarms: by guest address; an OSAlarm keeps its handler at +0 and the
// time it fires at +8, for code that reads them.
struct Alarm {
    uint64_t fire = 0, period = 0;
    uint32_t handler = 0;
    uint64_t generation = 0;
};
std::map<uint32_t, Alarm> g_alarms;
uint64_t g_generation = 0;
struct Wake {
    uint64_t at;
    uint32_t thread;
};
std::vector<Wake> g_sleepers;  // OSSleepTicks

void poll_alarms() {
    const uint64_t now = time_base();
    for (auto& [a, al] : g_alarms) {
        if (al.handler == 0 || al.fire > now) continue;
        const uint64_t gen = al.generation;
        const uint32_t handler = al.handler, alarm = a;
        if (al.period) {
            al.fire += al.period * ((now - al.fire) / al.period + 1);
            rt::poke32(a + 8, static_cast<uint32_t>(al.fire >> 32));
            rt::poke32(a + 12, static_cast<uint32_t>(al.fire));
        } else {
            al.handler = 0;  // one-shot: disarmed before its handler runs
        }
        post_interrupt([alarm, handler, gen](Cpu& c) {
            const auto it = g_alarms.find(alarm);
            if (it == g_alarms.end() || it->second.generation != gen) return;  // cancelled or set again since
            if (!it->second.period) rt::poke32(alarm, 0);
            guest_callback(c, handler, alarm, g_current ? g_current->guest : 0);
        });
    }
    for (size_t i = 0; i < g_sleepers.size();) {
        if (g_sleepers[i].at <= now) {
            const uint32_t t = g_sleepers[i].thread;
            g_sleepers.erase(g_sleepers.begin() + static_cast<long>(i));
            post_interrupt([t](Cpu& c) { wake_all(c, t + kQueue); });
        } else {
            ++i;
        }
    }
}
uint64_t alarm_deadline() {
    uint64_t t = UINT64_MAX;
    for (const auto& [a, al] : g_alarms)
        if (al.handler) t = std::min(t, al.fire);
    for (const auto& s : g_sleepers) t = std::min(t, s.at);
    return t;
}

void arm(uint32_t a, uint64_t fire, uint64_t period, uint32_t handler) {
    Alarm& al = g_alarms[a];
    al = {fire, period, handler, ++g_generation};
    rt::poke32(a, handler);
    rt::poke32(a + 8, static_cast<uint32_t>(fire >> 32));
    rt::poke32(a + 12, static_cast<uint32_t>(fire));
    rt::poke32(a + 0x18, static_cast<uint32_t>(period >> 32));
    rt::poke32(a + 0x1C, static_cast<uint32_t>(period));
}
uint64_t arg64(const Cpu& c, unsigned r) { return uint64_t{c.r[r]} << 32 | c.r[r + 1]; }

// ---- the API: threads
void OSGetCurrentThread(Cpu& c) { c.r[3] = g_current->guest; }

void OSCreateThread(Cpu& c) {  // (thread, func, param, stack, stackSize, priority, attr) -> BOOL
    const uint32_t t = c.r[3], func = c.r[4], param = c.r[5], stack = c.r[6], size = c.r[7], attr = c.r[9];
    const int32_t prio = static_cast<int32_t>(c.r[8]);
    if (prio < 0 || prio > 31) {
        c.r[3] = 0;
        return;
    }
    if (auto it = g_threads.find(t); it != g_threads.end()) {
        if (it->second->state != kMoribund && it->second->started)
            rt::fatal(&c, "OSCreateThread on %08X, a thread that is still alive", t);
        g_threads.erase(it);
    }
    auto th = std::make_unique<Thread>();
    th->guest = t;
    th->own = std::make_unique<Cpu>(rt::new_cpu());
    th->cpu = th->own.get();
    th->priority = prio;
    th->suspend = 1;
    th->state = kReady;
    th->detached = attr & 1;
    th->entry = func;
    Cpu& n = *th->cpu;
    const uint32_t top = stack & ~7u;
    rt::poke32(top - 8, 0);
    rt::poke32(top - 4, 0);
    n.r[1] = top - 8;
    n.r[2] = c.r[2];
    n.r[13] = c.r[13];
    n.r[3] = param;
    n.lr = kOSExitThread;
    if (rt::peek32(kFpuContextFlag) != 0)
        for (int i = 0; i < 32; ++i) n.ps0[i] = n.ps1[i] = std::bit_cast<double>(~uint64_t{0});
    // The guest's OSThread, as OSCreateThread fills it.
    std::memset(rt::guest_ptr(t + kState), 0, 0x318 - kState);
    rt::poke16(t + kAttr, static_cast<uint16_t>(attr & 1));
    rt::poke32(t + kBase, static_cast<uint32_t>(prio));
    rt::poke32(t + kVal, 0xFFFFFFFFu);
    rt::poke32(t + kStackBase, stack);
    rt::poke32(t + kStackEnd, stack - size);
    rt::poke32(stack - size, 0xDEADBABEu);
    Thread* p = th.get();
    g_threads[t] = std::move(th);
    store(p);
    c.r[3] = 1;
}

void OSResumeThread(Cpu& c) {  // (thread) -> the suspend count before
    Thread* t = thread_at(c, c.r[3]);
    const int was = t->suspend;
    if (t->suspend > 0 && --t->suspend == 0 && t->state == kReady) make_ready(t);
    store(t);
    c.r[3] = static_cast<uint32_t>(was);
    reschedule(c);
}

void OSSuspendThread(Cpu& c) {  // (thread) -> the suspend count before
    Thread* t = thread_at(c, c.r[3]);
    const int was = t->suspend++;
    c.r[3] = static_cast<uint32_t>(was);
    if (was == 0) {
        if (t == g_current) {
            t->state = kReady;  // ready, but held until resumed
            store(t);
            block(c);
            c.r[3] = static_cast<uint32_t>(was);
            return;
        }
        if (t->state == kReady) unready(t);
    }
    store(t);
}

void OSYieldThread(Cpu& c) {
    // A yield is how the guest waits for others in a loop: a safe point, so an
    // interrupt (a read finishing, the retrace) can arrive, as it would mid-loop.
    deliver_interrupts(c);
    Thread* me = g_current;
    Thread* best = best_ready();
    if (best && best->priority <= me->priority) {
        make_ready(me);
        switch_to(best);
    }
}

void OSExitThread(Cpu& c) {
    if (g_current->guest == kDefaultThread) rt::fatal(&c, "the main thread called OSExitThread");
    throw ThreadExit{c.r[3]};
}

void OSCancelThread(Cpu& c) {
    Thread* t = thread_at(c, c.r[3]);
    if (t == g_current) throw ThreadExit{0};
    if (t->state == kReady) unready(t);
    if (t->waiting_on) {
        auto& q = g_waiters[t->waiting_on];
        for (auto it = q.begin(); it != q.end(); ++it)
            if (*it == t) {
                q.erase(it);
                break;
            }
    }
    t->state = kMoribund;  // its host thread stays parked
    t->waiting_on = 0;
    store(t);
    wake_all(c, t->guest + kQueueJoin);
}

void OSJoinThread(Cpu& c) {  // (thread, val*) -> BOOL
    Thread* t = thread_at(c, c.r[3]);
    const uint32_t out = c.r[4];
    if (t->detached) {
        c.r[3] = 0;
        return;
    }
    while (t->state != kMoribund) sleep_on(c, t->guest + kQueueJoin);
    if (out) rt::poke32(out, rt::peek32(t->guest + kVal));
    c.r[3] = 1;
}

void OSSleepThread(Cpu& c) { sleep_on(c, c.r[3]); }
void OSWakeupThread(Cpu& c) { wake_all(c, c.r[3]); }
void OSInitThreadQueue(Cpu& c) {
    rt::poke32(c.r[3], 0);
    rt::poke32(c.r[3] + 4, 0);
}

void OSSleepTicks(Cpu& c) {  // (OSTime ticks): r3:r4
    const uint64_t ticks = arg64(c, 3);
    g_sleepers.push_back({time_base() + ticks, g_current->guest});
    sleep_on(c, g_current->guest + kQueue);
}

// ---- mutexes: queue (+0), owner (+8), count (+0xC). No priority inheritance yet.
void OSInitMutex(Cpu& c) {
    std::memset(rt::guest_ptr(c.r[3]), 0, 0x18);
    g_waiters.erase(c.r[3]);
}
void lock_mutex(Cpu& c, uint32_t m) {
    Thread* me = g_current;
    for (;;) {
        const uint32_t owner = rt::peek32(m + 8);
        if (owner == 0) {
            rt::poke32(m + 8, me->guest);
            rt::poke32(m + 0xC, 1);
            me->mutexes.push_back(m);
            return;
        }
        if (owner == me->guest) {
            rt::poke32(m + 0xC, rt::peek32(m + 0xC) + 1);
            return;
        }
        sleep_on(c, m);
    }
}
void release_mutex(Thread* me, uint32_t m) {
    rt::poke32(m + 8, 0);
    rt::poke32(m + 0xC, 0);
    std::erase(me->mutexes, m);
}
void OSLockMutex(Cpu& c) { lock_mutex(c, c.r[3]); }
void OSUnlockMutex(Cpu& c) {
    const uint32_t m = c.r[3];
    Thread* me = g_current;
    if (rt::peek32(m + 8) != me->guest) return;  // as the OS: only the owner unlocks
    const uint32_t n = rt::peek32(m + 0xC) - 1;
    if (n) {
        rt::poke32(m + 0xC, n);
        return;
    }
    release_mutex(me, m);
    wake_all(c, m);
}

// ---- condition variables: a thread queue.
void OSWaitCond(Cpu& c) {  // (cond, mutex)
    const uint32_t cond = c.r[3], m = c.r[4];
    Thread* me = g_current;
    if (rt::peek32(m + 8) != me->guest) return;
    const uint32_t count = rt::peek32(m + 0xC);
    // Releasing the mutex and going to sleep on the condition are one step: the
    // OS wakes the mutex's waiters with the scheduler off (OSDisableScheduler),
    // so none of them can signal before this thread is waiting.
    release_mutex(me, m);
    wake_waiters(m);
    sleep_on(c, cond);
    lock_mutex(c, m);
    rt::poke32(m + 0xC, count);
}

// ---- message queues: send queue (+0), receive queue (+8), msgArray (+0x10),
// msgCount (+0x14), firstIndex (+0x18), usedCount (+0x1C).
void OSInitMessageQueue(Cpu& c) {  // (mq, msgArray, msgCount)
    const uint32_t mq = c.r[3];
    std::memset(rt::guest_ptr(mq), 0, 0x20);
    rt::poke32(mq + 0x10, c.r[4]);
    rt::poke32(mq + 0x14, c.r[5]);
    g_waiters.erase(mq);
    g_waiters.erase(mq + 8);
}
void OSSendMessage(Cpu& c) {  // (mq, msg, flags) -> BOOL; flags & 1: block
    const uint32_t mq = c.r[3], msg = c.r[4], flags = c.r[5];
    const uint32_t count = rt::peek32(mq + 0x14);
    while (rt::peek32(mq + 0x1C) >= count) {
        if (!(flags & 1)) {
            c.r[3] = 0;
            return;
        }
        sleep_on(c, mq);
    }
    const uint32_t used = rt::peek32(mq + 0x1C);
    rt::poke32(rt::peek32(mq + 0x10) + 4 * ((rt::peek32(mq + 0x18) + used) % count), msg);
    rt::poke32(mq + 0x1C, used + 1);
    c.r[3] = 1;
    wake_all(c, mq + 8);
}
void OSJamMessage(Cpu& c) {  // (mq, msg, flags): to the front
    const uint32_t mq = c.r[3], msg = c.r[4], flags = c.r[5];
    const uint32_t count = rt::peek32(mq + 0x14);
    while (rt::peek32(mq + 0x1C) >= count) {
        if (!(flags & 1)) {
            c.r[3] = 0;
            return;
        }
        sleep_on(c, mq);
    }
    const uint32_t first = (rt::peek32(mq + 0x18) + count - 1) % count;
    rt::poke32(mq + 0x18, first);
    rt::poke32(rt::peek32(mq + 0x10) + 4 * first, msg);
    rt::poke32(mq + 0x1C, rt::peek32(mq + 0x1C) + 1);
    c.r[3] = 1;
    wake_all(c, mq + 8);
}
void OSReceiveMessage(Cpu& c) {  // (mq, msg*, flags) -> BOOL
    const uint32_t mq = c.r[3], out = c.r[4], flags = c.r[5];
    while (rt::peek32(mq + 0x1C) == 0) {
        if (!(flags & 1)) {
            c.r[3] = 0;
            return;
        }
        sleep_on(c, mq + 8);
    }
    const uint32_t first = rt::peek32(mq + 0x18);
    if (out) rt::poke32(out, rt::peek32(rt::peek32(mq + 0x10) + 4 * first));
    rt::poke32(mq + 0x18, (first + 1) % rt::peek32(mq + 0x14));
    rt::poke32(mq + 0x1C, rt::peek32(mq + 0x1C) - 1);
    c.r[3] = 1;
    wake_all(c, mq);
}

// ---- alarms
void OSCreateAlarm(Cpu& c) {
    g_alarms.erase(c.r[3]);
    rt::poke32(c.r[3], 0);
    rt::poke32(c.r[3] + 4, 0);
}
void OSSetAlarm(Cpu& c) { arm(c.r[3], time_base() + arg64(c, 5), 0, c.r[7]); }  // (alarm, ticks, handler)
void OSSetPeriodicAlarm(Cpu& c) {  // (alarm, start, period, handler)
    const uint64_t start = arg64(c, 5), period = arg64(c, 7), now = time_base();
    if (period == 0) rt::fatal(&c, "OSSetPeriodicAlarm with a period of 0");
    const uint64_t fire = start > now ? start : start + period * ((now - start) / period + 1);
    arm(c.r[3], fire, period, c.r[9]);
    rt::poke32(c.r[3] + 0x20, static_cast<uint32_t>(start >> 32));
    rt::poke32(c.r[3] + 0x24, static_cast<uint32_t>(start));
}
void OSCancelAlarm(Cpu& c) {
    g_alarms.erase(c.r[3]);
    rt::poke32(c.r[3], 0);
}
void OSSetAlarmUserData(Cpu& c) { rt::poke32(c.r[3] + 0x28, c.r[4]); }
void OSGetAlarmUserData(Cpu& c) { c.r[3] = rt::peek32(c.r[3] + 0x28); }

const rt::Provided kReplacements[] = {
    {0x80208024u, "OSGetCurrentThread", OSGetCurrentThread},
    {0x802085CCu, "OSCreateThread", OSCreateThread},
    {0x80208C3Cu, "OSResumeThread", OSResumeThread},
    {0x80208ED4u, "OSSuspendThread", OSSuspendThread},
    {0x80208590u, "OSYieldThread", OSYieldThread},
    {0x80208838u, "OSExitThread", OSExitThread},
    {0x8020891Cu, "OSCancelThread", OSCancelThread},
    {0x80208AFCu, "OSJoinThread", OSJoinThread},
    {0x80209068u, "OSSleepThread", OSSleepThread},
    {0x80209154u, "OSWakeupThread", OSWakeupThread},
    {0x80208014u, "OSInitThreadQueue", OSInitThreadQueue},
    {0x802092C8u, "OSSleepTicks", OSSleepTicks},
    {0x802064A4u, "OSInitMutex", OSInitMutex},
    {0x802064DCu, "OSLockMutex", OSLockMutex},
    {0x802065B8u, "OSUnlockMutex", OSUnlockMutex},
    {0x802066ECu, "OSInitCond", OSInitThreadQueue},
    {0x802066F0u, "OSWaitCond", OSWaitCond},
    {0x802067CCu, "OSSignalCond", OSWakeupThread},
    {0x80205A98u, "OSInitMessageQueue", OSInitMessageQueue},
    {0x80205AF8u, "OSSendMessage", OSSendMessage},
    {0x80205BC0u, "OSReceiveMessage", OSReceiveMessage},
    {0x80205C9Cu, "OSJamMessage", OSJamMessage},
    {0x801FFCE4u, "OSCreateAlarm", OSCreateAlarm},
    {0x801FFF44u, "OSSetAlarm", OSSetAlarm},
    {0x801FFFB4u, "OSSetPeriodicAlarm", OSSetPeriodicAlarm},
    {0x80200038u, "OSCancelAlarm", OSCancelAlarm},
    {0x80200458u, "OSSetAlarmUserData", OSSetAlarmUserData},
    {0x80200460u, "OSGetAlarmUserData", OSGetAlarmUserData},
};

}  // namespace

std::span<const rt::Provided> thread_replacements() { return kReplacements; }

void init_threads(Cpu& main_cpu) {
    auto th = std::make_unique<Thread>();
    th->guest = kDefaultThread;
    th->cpu = &main_cpu;
    th->priority = 16;
    th->state = kRunning;
    th->detached = true;
    th->started = true;
    g_current = th.get();
    // As __OSThreadInit leaves the default thread.
    const uint32_t t = kDefaultThread;
    std::memset(rt::guest_ptr(t + kState), 0, 0x318 - kState);
    rt::poke16(t + kAttr, 1);
    rt::poke32(t + kBase, 16);
    rt::poke32(t + kVal, 0xFFFFFFFFu);
    rt::poke32(t + kStackBase, 0x804292E8u);
    rt::poke32(t + kStackEnd, kMainStackEnd);
    rt::poke32(kMainStackEnd, 0xDEADBABEu);
    rt::poke32(kDefaultThreadPtr, t);
    rt::poke32(kCurrentThread, t);
    rt::poke32(kActiveHead, t);
    rt::poke32(kActiveTail, t);
    store(th.get());
    g_threads[t] = std::move(th);
    add_timer_source(poll_alarms, alarm_deadline);
}

void wake_all(Cpu& c, uint32_t queue) {
    if (wake_waiters(queue)) reschedule(c);
}

void wait_on(Cpu& c, uint32_t queue) { sleep_on(c, queue); }

void run_on_host_thread(void (*fn)(void*), void* arg) {
    struct Start {
        void (*fn)(void*);
        void* arg;
    };
    auto* s = new Start{fn, arg};
#if defined(_WIN32)
    HANDLE h = CreateThread(nullptr, kHostStack, [](LPVOID p) -> DWORD {
        std::unique_ptr<Start> st(static_cast<Start*>(p));
        st->fn(st->arg);
        return 0;
    }, s, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) rt::fatal(nullptr, "cannot start a host thread");
    CloseHandle(h);
#else
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, kHostStack);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t id;
    if (pthread_create(&id, &a, [](void* p) -> void* {
            std::unique_ptr<Start> st(static_cast<Start*>(p));
            st->fn(st->arg);
            return nullptr;
        }, s) != 0)
        rt::fatal(nullptr, "cannot start a host thread");
    pthread_attr_destroy(&a);
#endif
}

void preempt(Cpu& c) {
    if (!g_current) return;
    Thread* best = best_ready();
    if (best && best->priority < g_current->priority) {
        make_ready(g_current);
        switch_to(best);
    }
    (void)c;
}

void finish_thread(Cpu& c, uint32_t value) {
    Thread* me = g_current;
    rt::poke32(me->guest + kVal, value);
    // Wake without switching: a thread switched away from here would be put
    // back as ready, and a joiner would find it not yet finished.
    for (uint32_t m : std::vector(me->mutexes)) {  // the OS unlocks what an exiting thread holds
        release_mutex(me, m);
        wake_waiters(m);
    }
    me->state = kMoribund;
    store(me);
    wake_waiters(me->guest + kQueueJoin);
    block(c, true);  // the guest CPU belongs to another thread now; this host thread ends
}

}  // namespace punchout
