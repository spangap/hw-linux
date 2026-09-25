/**
 * tick.cpp — the FreeRTOS tick, suppressed while every task is blocked.
 *
 * The kernel is built tickless (esp-idf/CMakeLists.txt): when no task needs
 * the CPU for two ticks or more, the idle task calls
 * vPortSuppressTicksAndSleep() with the scheduler suspended, and the port
 * stops the tick, sleeps until the next task is due or an interrupt arrives,
 * and steps the tick count by what passed, the way a chip's tickless idle
 * does. So a quiet station costs nothing between the instants it has
 * something to do.
 *
 * Two clocks can drive the tick:
 *
 * - the host's (a real-time run). The tick is the port's 100 Hz setitimer.
 *   Idle stops it, sleeps in ppoll() until the next due tick or a signal —
 *   the descriptor controller's SIGUSR2 (fdwait.cpp) is one — steps the
 *   ticks that passed and restarts it on the same phase.
 *
 * - a clock another component keeps (hwlinux.h). The tick count is a
 *   function of that clock: the ticks fall on whole multiples of the tick
 *   period on it, and the clock calls hwLinuxClockMoved() each time it moves,
 *   which brings the count up to it on the spot. The port's setitimer is
 *   stopped for good. The clock is told the next tick anyone waits for
 *   (hwLinuxClockTickAt): while a task runs that is the next tick, and while
 *   every task is blocked it is the tick the first of them is due at. Idle
 *   says so (hwLinuxClockIdle) and sleeps until a signal: the clock's own
 *   message arriving on its descriptor is one.
 *
 * A pass of the idle task that the kernel did not hand to the suppress — a
 * task is due at the very next tick — sleeps until the next signal in the
 * idle hook instead, so the idle task never spins.
 *
 * Every wait here runs with signals blocked up to the ppoll() that opens
 * them, so an interrupt that arrives while the decision to sleep is being
 * taken ends the sleep rather than being lost: the host's WFI.
 */
#include "hwlinux.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <poll.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static_assert(configUSE_TICKLESS_IDLE != 0,
              "hw-linux builds the kernel tickless: see esp-idf/CMakeLists.txt");

extern "C" void vPortEnterCritical(void);
extern "C" void vPortExitCritical(void);

/* The station's clock, when a component in the image keeps it (hwlinux.h). */
extern "C" int     hwLinuxClockStart(void) __attribute__((weak));
extern "C" int64_t hwLinuxClockUs(void) __attribute__((weak));
extern "C" void    hwLinuxClockIdle(void) __attribute__((weak));
extern "C" void    hwLinuxClockTickAt(int64_t us) __attribute__((weak));

namespace {

constexpr int64_t    kTickUs = (int64_t)portTICK_PERIOD_MS * 1000;
constexpr int64_t    kNever  = INT64_MAX;
/* Beyond this many ticks a task is waiting for nothing: a deadline there is
 * none at all. */
constexpr TickType_t kFar    = (TickType_t)1 << 40;

/* The tick is the clock's: tick s_baseTick + n falls at s_baseUs + n·kTickUs,
 * s_baseUs a whole multiple of kTickUs. */
bool       s_clockTick = false;
int64_t    s_baseUs = 0;
TickType_t s_baseTick = 0;

/* Idle passes since the kernel last called the suppress. */
unsigned   s_passes = 0;

int64_t tickInstant(TickType_t tick)
{
    return s_baseUs + (int64_t)(tick - s_baseTick) * kTickUs;
}

int64_t monoUs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* With signals blocked: open them and wait for one, or for `us` to pass
 * (negative: no limit). */
void waitForInterrupt(int64_t us)
{
    sigset_t open;
    sigemptyset(&open);
    struct timespec ts, *tp = nullptr;
    if (us >= 0) {
        ts.tv_sec = (time_t)(us / 1000000);
        ts.tv_nsec = (long)(us % 1000000) * 1000;
        tp = &ts;
    }
    ppoll(nullptr, 0, tp, &open);
}

/* With the scheduler suspended: `passed` ticks went by while tasks due in
 * `expected` slept. Up to the first of them is a step; anything beyond is
 * held as ticks the kernel runs one by one when the scheduler resumes. */
void stepTicks(TickType_t passed, TickType_t expected)
{
    if (!passed) return;
    TickType_t step = passed < expected ? passed : expected;
    vTaskStepTick(step);
    for (TickType_t i = step; i < passed; i++) xTaskIncrementTick();
}

void tellNextTick()
{
    hwLinuxClockTickAt(tickInstant(xTaskGetTickCount() + 1));
}

/* On the host's clock. */
void hostIdle(TickType_t expected)
{
    vPortEnterCritical();
    struct itimerval off = {}, was = {};
    setitimer(ITIMER_REAL, &off, &was);
    int64_t left = (int64_t)was.it_value.tv_sec * 1000000 + was.it_value.tv_usec;
    if (left <= 0) left = kTickUs;

    sigset_t pending;
    sigpending(&pending);
    eSleepModeStatus status = eTaskConfirmSleepModeStatus();
    if (sigismember(&pending, SIGALRM) || status == eAbortSleep) {
        struct itimerval again = {};
        again.it_interval.tv_usec = kTickUs;
        again.it_value.tv_sec = (time_t)(left / 1000000);
        again.it_value.tv_usec = (suseconds_t)(left % 1000000);
        setitimer(ITIMER_REAL, &again, nullptr);
        vPortExitCritical();
        return;
    }

    int64_t now = monoUs();
    int64_t nextTick = now + left;
    int64_t wait = -1;
    if (status != eNoTasksWaitingTimeout && expected < kFar)
        wait = nextTick + (int64_t)(expected - 1) * kTickUs - now;
    waitForInterrupt(wait);

    int64_t after = monoUs();
    TickType_t passed = after >= nextTick ? (TickType_t)(1 + (after - nextTick) / kTickUs) : 0;
    stepTicks(passed, expected);

    int64_t next = nextTick + (int64_t)passed * kTickUs - after;
    if (next <= 0) next = 1;
    struct itimerval tick = {};
    tick.it_interval.tv_usec = kTickUs;
    tick.it_value.tv_sec = (time_t)(next / 1000000);
    tick.it_value.tv_usec = (suseconds_t)(next % 1000000);
    setitimer(ITIMER_REAL, &tick, nullptr);
    vPortExitCritical();
}

/* On a clock another component keeps. The count is already that clock's
 * (hwLinuxClockMoved), so there is nothing to step here. */
void clockIdle(TickType_t expected)
{
    vPortEnterCritical();
    eSleepModeStatus status = eTaskConfirmSleepModeStatus();
    if (status != eAbortSleep) {
        int64_t until = (status == eNoTasksWaitingTimeout || expected >= kFar)
                      ? kNever : tickInstant(xTaskGetTickCount() + expected);
        hwLinuxClockTickAt(until);
        if (hwLinuxClockIdle) hwLinuxClockIdle();
        waitForInterrupt(-1);
        tellNextTick();
    }
    vPortExitCritical();
}

}  // namespace

/* portSUPPRESS_TICKS_AND_SLEEP: from the idle task, the scheduler suspended,
 * no task due for `expected` ticks. */
extern "C" void vPortSuppressTicksAndSleep(TickType_t expected)
{
    s_passes = 0;
    if (s_clockTick) clockIdle(expected);
    else             hostIdle(expected);
}

/* The port's own idle hook sleeps 15 ms on every pass (port_idf.c); this
 * replaces it (-Wl,--wrap). The kernel calls it before deciding whether to
 * suppress the tick, so the first pass after the suppress returns goes on to
 * it, and a pass after one the kernel kept is a task due at the next tick:
 * sleep until an interrupt, the tick included. */
extern "C" void __wrap_esp_vApplicationIdleHook(void)
{
    if (s_passes++ == 0) return;
    vPortEnterCritical();
    if (s_clockTick && hwLinuxClockIdle) hwLinuxClockIdle();
    waitForInterrupt(-1);
    vPortExitCritical();
}

extern "C" void hwLinuxClockMoved(void)
{
    if (!s_clockTick || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return;
    int64_t us = hwLinuxClockUs();
    TickType_t count = xTaskGetTickCount();
    if (us >= s_baseUs) {
        TickType_t due = s_baseTick + (TickType_t)((us - s_baseUs) / kTickUs);
        if (due > count) xTaskCatchUpTicks(due - count);
    }
    tellNextTick();
}

/* From the board's bring-up: the clock comes up, and when it is one another
 * component keeps, the tick becomes its. */
void hwLinuxTickStart(void)
{
    if (!hwLinuxClockStart || !hwLinuxClockStart()) return;
    if (!hwLinuxClockUs || !hwLinuxClockTickAt) return;
    struct itimerval off = {};
    setitimer(ITIMER_REAL, &off, nullptr);
    vPortEnterCritical();
    int64_t now = hwLinuxClockUs();
    s_baseUs = now - now % kTickUs;
    s_baseTick = xTaskGetTickCount();
    s_clockTick = true;
    vPortExitCritical();
    tellNextTick();
}
