/**
 * hwlinux.h — the station's environment when the station is a Linux process.
 *
 * One process is one station. What a chip would have had burned into it or
 * wired to it comes from the environment instead, read once at the first
 * call and stable for the life of the process:
 *
 *   SPANGAP_NODE_ID     a small integer, the station's identity. It is the
 *                       last byte of the MAC the platform reads, so every
 *                       station on one host is a distinct device.
 *   SPANGAP_NODE_DIR    the station's directory. It is created, `state/` is
 *                       created under it, `fixed` is linked into it, and the
 *                       process chdir()s there — which is what makes the
 *                       platform's relative filesystem roots resolve.
 *   SPANGAP_BIND_ADDR   the loopback address this station's listeners bind,
 *                       127.0.0.1<id> by default, so every station keeps the
 *                       canonical port numbers.
 *   SPANGAP_ETHER       host:port of the virtual ether.
 *   SPANGAP_FIXED_DIR   the build's merged read-only data tree, linked in as
 *                       ./fixed.
 */
#pragma once

#include <sys/select.h>

#include "freertos/FreeRTOS.h"
#include "service.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * select() for a task that also waits on its own notifications: blocks until a
 * descriptor in the sets is ready, `ticks` pass, or the calling task is
 * notified on index 0, and returns what select() would — 0 with the sets
 * cleared for the last two. The notification's value is not taken, so the
 * task's own ulTaskNotifyTake still sees it.
 *
 * Plain select() blocks the calling task the same way (it is wrapped at link
 * time), without the notification. See src/fdwait.cpp.
 */
int hwLinuxWait(int nfds, fd_set* rfds, fd_set* wfds, fd_set* efds, TickType_t ticks);

/*
 * The station's clock, when a component in the image keeps it. The board
 * calls these if they are defined — it declares them weak — and runs on the
 * host's clocks if they are not:
 *
 *   hwLinuxClockStart()     from the board's onStart, before anything waits;
 *                           nonzero when the clock is not the host's, and the
 *                           tick is then stepped from it (src/tick.cpp)
 *   hwLinuxClockUs()        esp_timer_get_time(): µs on the station's clock
 *   hwLinuxClockWake(us)    esp_timer's earliest expiry moved (INT64_MAX: none)
 *   hwLinuxClockTickAt(us)  the next tick a task waits for (INT64_MAX: none)
 *   hwLinuxClockIdle()      every task is blocked until the earlier of those
 *                           two, or an interrupt
 *
 * and the board offers the clock these:
 *
 *   hwLinuxClockDue()       the clock reached the expiry it was last told
 *   hwLinuxClockMoved()     the clock moved: the tick count is brought up to
 *                           it. From a task, never from inside a critical
 *                           section, before anything due at the new instant
 *                           runs
 */
void hwLinuxClockDue(void);
void hwLinuxClockMoved(void);
int64_t hwLinuxClockUs(void);
void hwLinuxClockWake(int64_t us);
void hwLinuxClockTickAt(int64_t us);
int hwLinuxClockStart(void);
void hwLinuxClockIdle(void);

/** The station's identity. 1 when unset. */
int hwLinuxNodeId(void);

/** The loopback address every listener binds, in dotted-quad form. */
const char* hwLinuxBindAddr(void);

/** `host:port` of the virtual ether. Empty when no ether was named. */
const char* hwLinuxEtherAddr(void);

#ifdef __cplusplus
}

/**
 * Board bring-up. onStart() runs before spangapInit(): it makes the station's
 * directory, puts `state/` and the `fixed` link in it, and chdir()s there, so
 * the roots the platform opens by relative path land inside this station and
 * nowhere near another's.
 */
class HwLinuxBoard : public Service {
public:
    void onStart() override;
};
#endif
