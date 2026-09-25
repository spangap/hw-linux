/**
 * fdwait.cpp — an interrupt controller for file descriptors.
 *
 * On this port a FreeRTOS task that blocks in a host system call is, to the
 * scheduler, still running: nothing below it runs until the call returns. So
 * a task that waits for a socket or the console must block on a FreeRTOS
 * primitive, and something has to end that wait when the descriptor is ready,
 * the way an interrupt ends a wait on a chip.
 *
 *     task            select(): arm the descriptors, block on a notification
 *     epoll thread    epoll_wait() → ready → the waiters' bits → kill(SIGUSR2)
 *     signal          runs on the thread of whichever task is running, as the
 *                     tick's SIGALRM does → the waiters' notifications FromISR,
 *                     and a switch when one of them outranks the running task
 *     task            woken: disarm, read the sets with a zero-timeout pselect
 *
 * The epoll thread is an ordinary pthread outside FreeRTOS and never calls a
 * kernel API: it only sets bits and raises the signal. It is started before
 * main() with every signal blocked, so the signal can only land on a task
 * thread, and on this port only the running task's thread has signals
 * unblocked — every other task thread is suspended inside a critical section
 * or a signal handler. A task in a critical section has them blocked too, so
 * the signal waits for it to leave, exactly as the tick does, and the handler
 * and the task-side bookkeeping (done inside critical sections) never run at
 * once. The handler holds the critical-section count up while it calls the
 * kernel, as the port's tick handler does, and yields the way that handler
 * does.
 *
 * Descriptors are registered one-shot, only for the length of a wait, so a
 * descriptor that stays readable while its task works raises nothing more.
 *
 * Two waits share this:
 *
 * - select(), for every caller in the image (-Wl,--wrap=select replaces IDF's
 *   polling interposer): POSIX semantics, the task blocked on notification
 *   index 1, which nothing else uses;
 * - hwLinuxWait(), select() that also returns when the task is notified on
 *   index 0 — ITS, and every other xTaskNotifyGive, lands there — without
 *   taking the notification's count, so a task that sleeps in it still sees
 *   the notification in its own take afterwards. A ready descriptor posts a
 *   notification that carries no value to such a task.
 */
#include "hwlinux.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static_assert(configTASK_NOTIFICATION_ARRAY_ENTRIES >= 2,
              "hw-linux waits on notification index 1: "
              "CONFIG_FREERTOS_TASK_NOTIFICATION_ARRAY_ENTRIES must be at least 2");

extern "C" void vPortYieldFromISR(void);
extern "C" void vPortEnterCritical(void);
extern "C" void vPortExitCritical(void);

namespace {

constexpr int         kMaxFd        = FD_SETSIZE;
constexpr int         kMaxWaiters   = 64;
constexpr int         kIrqSignal    = SIGUSR2;
constexpr UBaseType_t kSelectIndex  = 1;
constexpr UBaseType_t kNotifyIndex  = 0;

struct Waiter {
    TaskHandle_t task;
    UBaseType_t  index;
    bool         waiting;
};

Waiter                 s_waiters[kMaxWaiters];
std::atomic<uint64_t>  s_fdWaiters[kMaxFd];
uint32_t               s_fdEvents[kMaxFd];
std::atomic<uint64_t>  s_pending{0};
int                    s_epfd = -1;
pid_t                  s_pid;
bool                   s_up = false;

void fdIrq(int)
{
    int saved = errno;
    /* Suspended is not stopped: the FromISR calls queue the woken task, and
     * the switch happens when the scheduler resumes, as for any interrupt. */
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) { errno = saved; return; }
    vPortEnterCritical();
    uint64_t bits = s_pending.exchange(0);
    BaseType_t woken = pdFALSE;
    for (int i = 0; bits && i < kMaxWaiters; i++, bits >>= 1) {
        if (!(bits & 1)) continue;
        Waiter& w = s_waiters[i];
        if (!w.waiting || !w.task) continue;
        if (w.index == kSelectIndex)
            vTaskNotifyGiveIndexedFromISR(w.task, kSelectIndex, &woken);
        else
            xTaskNotifyIndexedFromISR(w.task, kNotifyIndex, 0, eNoAction, &woken);
    }
    if (woken) vPortYieldFromISR();
    vPortExitCritical();
    errno = saved;
}

void* epollThread(void*)
{
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, nullptr);
    struct epoll_event ev[64];
    for (;;) {
        int n = epoll_wait(s_epfd, ev, 64, -1);
        if (n <= 0) continue;
        uint64_t bits = 0;
        for (int i = 0; i < n; i++) {
            int fd = ev[i].data.fd;
            if (fd >= 0 && fd < kMaxFd) bits |= s_fdWaiters[fd].load();
        }
        if (bits) {
            s_pending.fetch_or(bits);
            kill(s_pid, kIrqSignal);
        }
    }
    return nullptr;
}

__attribute__((constructor)) void fdWaitStart()
{
    s_pid = getpid();
    s_epfd = epoll_create1(EPOLL_CLOEXEC);
    if (s_epfd < 0) {
        fprintf(stderr, "hw-linux: epoll_create1: %s\n", strerror(errno));
        return;
    }
    struct sigaction sa = {};
    sa.sa_handler = fdIrq;
    sigfillset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(kIrqSignal, &sa, nullptr) != 0) {
        fprintf(stderr, "hw-linux: sigaction: %s\n", strerror(errno));
        return;
    }
    pthread_t t;
    if (pthread_create(&t, nullptr, epollThread, nullptr) != 0) {
        fprintf(stderr, "hw-linux: no descriptor thread\n");
        return;
    }
    pthread_detach(t);
    s_up = true;
}

int slotFor(TaskHandle_t task)
{
    int free = -1;
    for (int i = 0; i < kMaxWaiters; i++) {
        if (s_waiters[i].task == task) return i;
        if (free < 0 && !s_waiters[i].task) free = i;
    }
    if (free >= 0) s_waiters[free].task = task;
    return free;
}

struct Sets {
    int    nfds;
    fd_set r, w, e;
    bool   hasR, hasW, hasE;

    Sets(int n, fd_set* rp, fd_set* wp, fd_set* ep) : nfds(n), hasR(rp), hasW(wp), hasE(ep)
    {
        if (rp) r = *rp; else FD_ZERO(&r);
        if (wp) w = *wp; else FD_ZERO(&w);
        if (ep) e = *ep; else FD_ZERO(&e);
    }

    int check(fd_set* rp, fd_set* wp, fd_set* ep) const
    {
        for (;;) {
            if (rp) *rp = r;
            if (wp) *wp = w;
            if (ep) *ep = e;
            struct timespec zero = {0, 0};
            int n = pselect(nfds, rp, wp, ep, &zero, nullptr);
            if (n >= 0 || errno != EINTR) return n;
        }
    }

    void clear(fd_set* rp, fd_set* wp, fd_set* ep) const
    {
        if (rp) FD_ZERO(rp);
        if (wp) FD_ZERO(wp);
        if (ep) FD_ZERO(ep);
    }

    uint32_t events(int fd) const
    {
        uint32_t ev = 0;
        if (hasR && FD_ISSET(fd, &r)) ev |= EPOLLIN | EPOLLRDHUP;
        if (hasW && FD_ISSET(fd, &w)) ev |= EPOLLOUT;
        if (hasE && FD_ISSET(fd, &e)) ev |= EPOLLPRI;
        return ev;
    }
};

void arm(int slot, const Sets& s, UBaseType_t index)
{
    uint64_t bit = 1ull << slot;
    vPortEnterCritical();
    s_waiters[slot].index = index;
    s_waiters[slot].waiting = true;
    if (index == kSelectIndex) {
        xTaskNotifyStateClearIndexed(nullptr, kSelectIndex);
        ulTaskNotifyValueClearIndexed(nullptr, kSelectIndex, ~0u);
    }
    for (int fd = 0; fd < s.nfds; fd++) {
        uint32_t ev = s.events(fd);
        if (!ev) continue;
        uint64_t before = s_fdWaiters[fd].fetch_or(bit);
        s_fdEvents[fd] = (before & ~bit) ? (s_fdEvents[fd] | ev) : ev;
        struct epoll_event e = {};
        e.events = s_fdEvents[fd] | EPOLLONESHOT;
        e.data.fd = fd;
        if (epoll_ctl(s_epfd, EPOLL_CTL_MOD, fd, &e) != 0 && errno == ENOENT)
            epoll_ctl(s_epfd, EPOLL_CTL_ADD, fd, &e);
    }
    vPortExitCritical();
}

void disarm(int slot, const Sets& s)
{
    uint64_t bit = 1ull << slot;
    vPortEnterCritical();
    for (int fd = 0; fd < s.nfds; fd++)
        if (s.events(fd)) s_fdWaiters[fd].fetch_and(~bit);
    s_waiters[slot].waiting = false;
    vPortExitCritical();
}

/* Before the scheduler, on a thread that is not a task, or with every slot
 * taken: the port's own way, a zero-timeout check and a delay. */
int pollingWait(const Sets& s, fd_set* rp, fd_set* wp, fd_set* ep, TickType_t ticks)
{
    bool tasks = xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
    if (!tasks) {
        if (rp) *rp = s.r;
        if (wp) *wp = s.w;
        if (ep) *ep = s.e;
        struct timespec ts, *tp = nullptr;
        if (ticks != portMAX_DELAY) {
            uint64_t us = (uint64_t)ticks * portTICK_PERIOD_MS * 1000;
            ts.tv_sec = (time_t)(us / 1000000);
            ts.tv_nsec = (long)(us % 1000000) * 1000;
            tp = &ts;
        }
        return pselect(s.nfds, rp, wp, ep, tp, nullptr);
    }
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        int n = s.check(rp, wp, ep);
        if (n != 0) return n;
        TickType_t spent = xTaskGetTickCount() - start;
        if (ticks != portMAX_DELAY && spent >= ticks) { s.clear(rp, wp, ep); return 0; }
        vTaskDelay(1);
    }
}

int waitOn(int nfds, fd_set* rp, fd_set* wp, fd_set* ep, TickType_t ticks, UBaseType_t index)
{
    if (nfds < 0 || nfds > kMaxFd) { errno = EINVAL; return -1; }
    Sets s(nfds, rp, wp, ep);
    int n = s.check(rp, wp, ep);
    if (n != 0 || ticks == 0) return n;

    if (!s_up || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)
        return pollingWait(s, rp, wp, ep, ticks);
    vPortEnterCritical();
    int slot = slotFor(xTaskGetCurrentTaskHandle());
    vPortExitCritical();
    if (slot < 0) return pollingWait(s, rp, wp, ep, ticks);

    TickType_t start = xTaskGetTickCount();
    for (;;) {
        arm(slot, s, index);
        n = s.check(rp, wp, ep);
        if (n != 0) { disarm(slot, s); return n; }
        TickType_t left = portMAX_DELAY;
        if (ticks != portMAX_DELAY) {
            TickType_t spent = xTaskGetTickCount() - start;
            if (spent >= ticks) { disarm(slot, s); s.clear(rp, wp, ep); return 0; }
            left = ticks - spent;
        }
        bool notified;
        if (index == kSelectIndex) {
            ulTaskNotifyTakeIndexed(kSelectIndex, pdTRUE, left);
            notified = false;
        } else {
            notified = xTaskNotifyWaitIndexed(kNotifyIndex, 0, 0, nullptr, left) == pdTRUE;
        }
        disarm(slot, s);
        n = s.check(rp, wp, ep);
        if (n != 0) return n;
        if (notified) return 0;
        if (ticks != portMAX_DELAY && xTaskGetTickCount() - start >= ticks) return 0;
    }
}

TickType_t ticksOf(const struct timeval* tv)
{
    if (!tv) return portMAX_DELAY;
    uint64_t us = (uint64_t)tv->tv_sec * 1000000 + (uint64_t)tv->tv_usec;
    uint64_t per = (uint64_t)portTICK_PERIOD_MS * 1000;
    return (TickType_t)((us + per - 1) / per);
}

}  // namespace

extern "C" int hwLinuxWait(int nfds, fd_set* rfds, fd_set* wfds, fd_set* efds, TickType_t ticks)
{
    return waitOn(nfds, rfds, wfds, efds, ticks, kNotifyIndex);
}

extern "C" int __wrap_select(int nfds, fd_set* rfds, fd_set* wfds, fd_set* efds,
                             struct timeval* timeout)
{
    return waitOn(nfds, rfds, wfds, efds, ticksOf(timeout), kSelectIndex);
}
