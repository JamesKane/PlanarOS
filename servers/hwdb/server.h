/*
 * servers/hwdb/server.h -- hwdb as a server: its drivers, as it sees them
 *
 * A unit is one driver process and what it's leased: one hw_lease, so
 * every device in it is served by that process (lease.c). server.c
 * starts, watches and restarts units; killtest.c is its test mode.
 */
#ifndef HWDB_SERVER_H
#define HWDB_SERVER_H

#include <planar/drv.h>
#include <planar/exec.h>
#include <planar/lease.h>
#include <planar/msg.h>

#include "hwdb.h"

/* Supervision (VQ23): the first restart after BACKOFF_MS, each next one
 * twice as late; given up at FAILS failures within WINDOW_MS. A unit
 * that hasn't said it started within START_MS has failed. */
constexpr unsigned FAILS = 5;
constexpr uint64_t WINDOW_MS = 10000;
constexpr uint64_t BACKOFF_MS = 10;
constexpr uint64_t START_MS = 2000;
constexpr uint64_t MS = 1000000;

/* The argument's bits, from the kernel. */
constexpr uint64_t HWDB_TEST = 1u << 0;     /* run the kill test, then exit */

enum unit_state : uint8_t {
    U_WAITING,          /* not all its devices ready */
    U_DUE,              /* to start at 'due' */
    U_STARTING,         /* started; not yet HWDB_STARTED (until 'due') */
    U_RUNNING,
    U_DONE,             /* declined everything, failed or given up: not restarted */
};

struct unit {
    enum unit_state state;
    bool killing;               /* hwdb killed it: an unbind, not a failure */
    bool late;                  /* killed for not starting in time */
    uint8_t ready;              /* its devices ready */
    uint8_t nfails;             /* failures in fail_at */
    uint32_t flags;             /* DRVF_*, for its next start */
    uint32_t gen;               /* its token's: a stale token is refused */
    uint32_t accepted;          /* HWDB_STARTED's, a bit per device */
    Handle lease[HW_LEASE_RANGES];
    Handle proc;
    uint64_t due, started;
    uint64_t dma;               /* pages in DMA Areas when hwdb killed it running */
    uint64_t fail_at[FAILS];    /* the latest failures, oldest first */
    char ports[DRV_DEVICES][16];
    unsigned starts, deaths;
};

struct hwdb {
    struct hw_graph g;
    struct hw_arena arena;
    Handle root, port;
    struct unit *units;         /* one per g.leases */
    uint64_t now;
    bool test, stopping, finished;
    unsigned errors;            /* teardown checks that failed */
    uint64_t quarantined;       /* DMA pages dead drivers left, all told */
};

extern struct hwdb hwdb;

const char *unit_driver(const struct unit *u);
const char *unit_path(const struct unit *u, struct hw_line *l);
bool hwdb_busy(void);           /* a unit is starting, due to, or ending */
void unit_kill(struct unit *u); /* an unbind: restarted at once unless stopping */
void unit_retry(struct unit *u);    /* start one given up on, afresh */
void say(const char *text);
void hwdb_report(void);      /* the lines worth the console */
void say_unit(const struct unit *u, const char *text);
void check_failed(const struct unit *u, const char *what);

/* The kill test (killtest.c): a step each time round the loop, and when
 * it next wants one (0: no time). */
void test_step(void);
uint64_t test_deadline(void);

#endif /* HWDB_SERVER_H */
