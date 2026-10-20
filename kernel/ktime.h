/*
 * kernel/ktime.h -- the monotonic clock and kernel timers
 *
 * Time is counted in timebase ticks (time_hz() per second). There is no
 * periodic tick: the hardware timer is armed for the earliest pending
 * kernel timer and nothing else. One timer list for now; V1 step 2 gives
 * each CPU its own.
 */
#ifndef KERNEL_KTIME_H
#define KERNEL_KTIME_H

#include <stdint.h>

/* Read the timebase rate and hook up the timer interrupt. False if
 * there's no timer interrupt; the clock still reads. */
bool time_init(void);
uint64_t time_hz(void);

/* Whether kernel timers fire: time_init() found a timer interrupt. */
bool time_have_timers(void);

/* Ticks since some point before boot. Never goes backwards. */
uint64_t time_now(void);

/* Convert microseconds to ticks, and ticks to microseconds. */
uint64_t time_us(uint64_t us);
uint64_t time_to_us(uint64_t ticks);

/* A one-shot kernel timer: fn(data) runs in the timer interrupt once
 * time_now() reaches deadline. The struct belongs to the caller. */
struct ktimer {
    uint64_t deadline;
    void (*fn)(void *data);
    void *data;
    struct ktimer *next;        /* pending list, sorted by deadline */
    bool pending;
};

void ktimer_start(struct ktimer *t, uint64_t deadline);
void ktimer_stop(struct ktimer *t);

/* Timer interrupts taken so far. */
uint64_t time_interrupts(void);

/* Called from the timer interrupt. */
void time_expired(void);

#endif /* KERNEL_KTIME_H */
