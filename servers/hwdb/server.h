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
#include <planar/devices/power.h>

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
constexpr uint64_t HWDB_END = 1u << 1;      /* not the test: end when the Shell does */

enum unit_state : uint8_t {
    U_WAITING,          /* not all its devices ready */
    U_DUE,              /* to start at 'due' */
    U_POWERING,         /* power is asked to vote for it; started on the answer (power.c) */
    U_STARTING,         /* started; not yet HWDB_STARTED (until 'due') */
    U_RUNNING,
    U_DONE,             /* declined everything, failed or given up: not restarted */
};

struct unit {
    enum unit_state state;
    bool killing;               /* hwdb killed it: an unbind, not a failure */
    bool late;                  /* killed for not starting in time */
    bool asked;                 /* U_POWERING: its vote is sent, the answer to come */
    bool voted;                 /* power holds a vote for it */
    bool revoting;              /* running, voted for again after power restarted */
    bool powered;               /* turned_on holds what its first vote since power started
                                 * turned on */
    uint32_t turned_on;
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
    uint64_t rsdp;              /* ACPI's, else 0: acpid runs */
    struct FirmwareInfo fb;     /* MapFirmware's: the GOP framebuffer, if any */
    uint64_t now;
    bool test, end, stopping, finished;
    unsigned errors;            /* teardown checks that failed */
    uint64_t quarantined;       /* DMA pages dead drivers left, all told */
};

extern struct hwdb hwdb;

const char *unit_driver(const struct unit *u);
const char *unit_path(const struct unit *u, struct hw_line *l);
bool hwdb_busy(void);           /* a unit is starting, due to, or ending; or acpid probing */
void unit_launch(struct unit *u);   /* grant its leases and start it: power is done */
void unit_fail(struct unit *u, const char *why);    /* its devices fail; not restarted */
void unit_kill(struct unit *u); /* an unbind: restarted at once unless stopping */
void unit_retry(struct unit *u);    /* start one given up on, afresh */
void say(const char *text);
void hwdb_report(void);      /* the lines worth the console */
void hwdb_facts(void);       /* what the drivers bound, failed and declined: "hw: " lines */
void say_unit(const struct unit *u, const char *text);
void check_failed(const struct unit *u, const char *what);

/* --- acpid (acpid.c). --- */

constexpr int64_t SIG_CONN = 17;            /* a driver's connection, or hwdb's to power */
constexpr int64_t SIG_DEAD = 18;            /* a unit's process died, or acpid */
constexpr int64_t SIG_DOS_CLIENT = 25;      /* hwdb's own connection to dos, for the Shell */
constexpr int64_t SIG_ACPI_PORT = 24;       /* acpid at "hwdb.acpi" */
constexpr uint32_t CONN_ACPID = UINT32_MAX - 1;     /* conn_unit's mark for its connection */
constexpr unsigned ACPID_REGIONS = 64;
constexpr unsigned ACPID_TABLES_MAX = 72;

struct acpid_region {
    uint64_t base, size;
    Handle lease;
};

struct acpid {
    Handle proc, port;
    uint32_t token;
    bool done, stopping, probed;
    uint64_t rsdp, until;
    unsigned devices, asked, refused;
    struct { uint64_t base, size; } tables[ACPID_TABLES_MAX];  /* the firmware's, mapped */
    unsigned ntables;
    struct acpid_region regions[ACPID_REGIONS];
    unsigned nregions;
};

extern struct acpid acpid;

bool acpid_start(uint64_t rsdp);    /* false (having said why) if it couldn't be */
bool acpid_waiting(void);           /* started, not done, not late */
void acpid_serve(struct MsgPort *mp);
bool acpid_reap(void);              /* true if it had died: its leases are taken back */
void acpid_finish(void);            /* its devices are all in: resolve them, say so */
void acpid_stop(void);              /* at the end: kill it, take its leases back */

/* --- Connections hwdb makes itself (server.c keeps them with the rest). --- */

constexpr uint32_t CONN_POWER = UINT32_MAX - 2;     /* conn_unit's mark for power's */
bool server_add_conn(const struct MsgPort *mp, uint32_t mark);
struct MsgPort *server_conn(uint32_t mark);         /* nullptr if there's none */

/* --- power (power.c): power before lease. --- */

struct power {
    bool up;                    /* connected to power's port */
    bool model;                 /* test mode added QEMU's model (power_model) */
    bool consumer;              /* and a driver that needs it */
    unsigned ups, downs, votes; /* connections, losses of one, votes answered */
    unsigned checked;           /* re-votes after a death that turned on what the first did */
};

extern struct power power;

bool power_needed(const struct unit *u);
void power_vote(struct unit *u);    /* U_POWERING; unit_launch once power has voted */
void power_release(struct unit *u); /* its teardown is done: the vote goes */
void power_serve(struct MsgPort *mp);
void power_up(void);                /* power's unit is running: connect and vote */
void power_down(void);              /* its connection went */
void power_model(void);             /* test mode, before matching */
/* The kill test's own: a named set queried, voted or let go; the answer
 * comes to test_power_answer, with 'tag' (above the units'). */
int64_t power_query(const char *name, uint32_t tag);
int64_t power_vote_set(const char *name, uint32_t tag, bool on);
void test_power_answer(const struct PowerMsg *m);
bool power_test(void);              /* the kill test's power step: true when it's over */

/* --- pci (pci.c): a host bridge's driver, while it binds. --- */

bool pci_host(const struct unit *u, struct HwdbMsg *m);            /* fills m->hm_Host */
int64_t pci_function(const struct unit *u, const struct HwdbMsg *m);  /* 0, or why not */

/* The kill test (killtest.c): a step each time round the loop, and when
 * it next wants one (0: no time). */
void test_step(void);
uint64_t test_deadline(void);

#endif /* HWDB_SERVER_H */
