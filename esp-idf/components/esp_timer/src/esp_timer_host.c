/**
 * esp_timer_host.c — esp_timer on the host.
 *
 * The clock is CLOCK_MONOTONIC in microseconds, zeroed at the first reading,
 * so a station's timestamps start near zero the way a chip's do and only ever
 * mean something relative to each other.
 *
 * The timers are one sorted list under a mutex and one task that sleeps in
 * ulTaskNotifyTake until the earliest expiry, runs whatever is due on its own
 * stack, and is notified whenever a start or a stop moves that instant. The
 * task exists from the first timer created — nothing on this target runs the
 * chip's startup code that would otherwise have brought it up.
 *
 * ESP_TIMER_ISR is dispatched as ESP_TIMER_TASK: there is no interrupt
 * context here, and a callback that expects one would be wrong about a great
 * deal more than which stack it is on.
 */
#include "esp_timer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

struct esp_timer {
    esp_timer_cb_t     cb;
    void*              arg;
    const char*        name;
    uint64_t           period;   /* 0 for a one-shot */
    uint64_t           next;     /* absolute expiry, µs; valid while active */
    bool               active;
    struct esp_timer*  link;     /* next in the sorted list */
};

static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_task;
static struct esp_timer* s_head;   /* active timers, earliest first */

#define TIMER_TASK_STACK  (32 * 1024)
#define TIMER_TASK_PRIO   22

/* The station's clock, when a component in the image keeps it (hwlinux.h). */
extern int64_t hwLinuxClockUs(void) __attribute__((weak));
extern void    hwLinuxClockWake(int64_t us) __attribute__((weak));

int64_t esp_timer_get_time(void)
{
    static int64_t origin;
    struct timespec ts;
    int64_t now;

    if (hwLinuxClockUs) return hwLinuxClockUs();
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    if (origin == 0) origin = now;
    return now - origin;
}

/* With the lock held: the clock learns the earliest expiry. */
static void tell_clock(void);

static void timer_task(void* unused);

/* The lock and the task come up on the first use. The first use is on the main
 * task before anything else runs, so the check needs no lock of its own. */
static void ensure_up(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_task) {
        xTaskCreate(timer_task, "esp_timer", TIMER_TASK_STACK / sizeof(StackType_t),
                    NULL, TIMER_TASK_PRIO, &s_task);
    }
}

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

/* Both list operations assume the lock is held. */
static void list_remove(struct esp_timer* t)
{
    struct esp_timer** p = &s_head;
    while (*p) {
        if (*p == t) { *p = t->link; t->link = NULL; return; }
        p = &(*p)->link;
    }
}

static void list_insert(struct esp_timer* t)
{
    struct esp_timer** p = &s_head;
    while (*p && (*p)->next <= t->next) p = &(*p)->link;
    t->link = *p;
    *p = t;
}

static void wake_task(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

static void tell_clock(void)
{
    if (hwLinuxClockWake) hwLinuxClockWake(s_head ? (int64_t)s_head->next : INT64_MAX);
}

void hwLinuxClockDue(void)
{
    wake_task();
}

static void arm(struct esp_timer* t, uint64_t timeout_us, uint64_t period)
{
    lock();
    if (t->active) list_remove(t);
    t->period = period;
    t->next   = (uint64_t)esp_timer_get_time() + timeout_us;
    t->active = true;
    list_insert(t);
    tell_clock();
    unlock();
    wake_task();
}

static void timer_task(void* unused)
{
    (void)unused;
    for (;;) {
        uint64_t now, due = 0;
        struct esp_timer* fire = NULL;
        esp_timer_cb_t cb = NULL;
        void* arg = NULL;
        TickType_t wait;

        lock();
        now = (uint64_t)esp_timer_get_time();
        if (s_head && s_head->next <= now) {
            fire = s_head;
            list_remove(fire);
            if (fire->period) {
                fire->next = now + fire->period;
                list_insert(fire);
            } else {
                fire->active = false;
            }
            cb  = fire->cb;
            arg = fire->arg;
            tell_clock();
        } else if (s_head) {
            due = s_head->next;
        }
        unlock();

        if (cb) { cb(arg); continue; }

        if (due) {
            uint64_t left = due - now;
            /* Round up: a callback may run late, never early. */
            wait = (TickType_t)((left + portTICK_PERIOD_MS * 1000 - 1) /
                                (portTICK_PERIOD_MS * 1000));
            if (wait == 0) wait = 1;
        } else {
            wait = portMAX_DELAY;
        }
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

esp_err_t esp_timer_early_init(void) { ensure_up(); return ESP_OK; }
esp_err_t esp_timer_init(void)       { ensure_up(); return ESP_OK; }
esp_err_t esp_timer_deinit(void)     { return ESP_OK; }

esp_err_t esp_timer_create(const esp_timer_create_args_t* create_args,
                           esp_timer_handle_t* out_handle)
{
    struct esp_timer* t;

    if (!create_args || !create_args->callback || !out_handle) return ESP_ERR_INVALID_ARG;
    ensure_up();

    t = (struct esp_timer*)calloc(1, sizeof(*t));
    if (!t) return ESP_ERR_NO_MEM;
    t->cb   = create_args->callback;
    t->arg  = create_args->arg;
    t->name = create_args->name;
    *out_handle = t;
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us)
{
    if (!timer) return ESP_ERR_INVALID_ARG;
    if (timer->active) return ESP_ERR_INVALID_STATE;
    arm(timer, timeout_us, 0);
    return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period)
{
    if (!timer || period == 0) return ESP_ERR_INVALID_ARG;
    if (timer->active) return ESP_ERR_INVALID_STATE;
    arm(timer, period, period);
    return ESP_OK;
}

esp_err_t esp_timer_restart(esp_timer_handle_t timer, uint64_t timeout_us)
{
    if (!timer) return ESP_ERR_INVALID_ARG;
    if (!timer->active) return ESP_ERR_INVALID_STATE;
    arm(timer, timeout_us, timer->period);
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    if (!timer) return ESP_ERR_INVALID_ARG;
    lock();
    if (timer->active) { list_remove(timer); timer->active = false; tell_clock(); }
    unlock();
    wake_task();
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    if (!timer) return ESP_ERR_INVALID_ARG;
    esp_timer_stop(timer);
    free(timer);
    return ESP_OK;
}

bool esp_timer_is_active(esp_timer_handle_t timer)
{
    return timer && timer->active;
}

esp_err_t esp_timer_get_period(esp_timer_handle_t timer, uint64_t* period)
{
    if (!timer || !period) return ESP_ERR_INVALID_ARG;
    *period = timer->period;
    return ESP_OK;
}

esp_err_t esp_timer_get_expiry_time(esp_timer_handle_t timer, uint64_t* expiry)
{
    if (!timer || !expiry) return ESP_ERR_INVALID_ARG;
    if (timer->period) return ESP_ERR_NOT_SUPPORTED;
    *expiry = timer->next;
    return ESP_OK;
}

int64_t esp_timer_get_next_alarm(void)
{
    int64_t next;
    lock();
    next = s_head ? (int64_t)s_head->next : INT64_MAX;
    unlock();
    return next;
}

int64_t esp_timer_get_next_alarm_for_wake_up(void)
{
    return esp_timer_get_next_alarm();
}

esp_err_t esp_timer_dump(FILE* stream)
{
    struct esp_timer* t;
    lock();
    for (t = s_head; t; t = t->link) {
        fprintf(stream ? stream : stdout, "%-20s period %llu next %llu\n",
                t->name ? t->name : "", (unsigned long long)t->period,
                (unsigned long long)t->next);
    }
    unlock();
    return ESP_OK;
}
