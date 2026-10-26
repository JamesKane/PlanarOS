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
constexpr uint64_t HWDB_FAULT = 1u << 2;    /* fault at once: init's test (servers/init) */
constexpr uint64_t HWDB_LOOK = 1u << 3;     /* the window test holds its windows on the screen to be looked at */
constexpr uint64_t HWDB_DRAG = 1u << 4;     /* the window test gives someone windows to drag, 60 s */
constexpr uint64_t HWDB_NOINTUITION = 1u << 5;  /* not the test: no desktop; the Shell has the keyboard */

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
    bool stopping;              /* asked to stop (its HWDB_STOP answered), by stop_due */
    bool stopped;               /* it did: exited 0 in time */
    struct HwdbMsg *stop_msg;   /* its HWDB_STOP, held: in its connection's memory */
    uint64_t stop_due;
    uint64_t reported;          /* when a client last said it missed a deadline */
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
    bool test, end, look, drag, stopping, finished;
    bool desktop;               /* not the test: intuition started once there's a screen (V8 step 4) */
    unsigned errors;            /* teardown checks that failed */
    uint64_t quarantined;       /* DMA pages dead drivers left, all told */
    Handle dos;                 /* the dos server, the kill test's or the system's */
    unsigned stops, stops_clean, stops_late;    /* units asked to stop; that did; killed */
    unsigned unresponsive;      /* servers restarted on a client's report */
    uint64_t marks;             /* their tasks said first (say_tasks) */
    uint64_t stopped_pages;     /* DMA pages quarantined from units that stopped as asked */
    unsigned stop_deps;         /* while stopping: dependencies whose consumer ended first */
};

extern struct hwdb hwdb;

const char *unit_driver(const struct unit *u);
const char *unit_path(const struct unit *u, struct hw_line *l);
bool hwdb_busy(void);           /* a unit is starting, due to, or ending; or acpid probing */
void unit_launch(struct unit *u);   /* grant its leases and start it: power is done */
void unit_fail(struct unit *u, const char *why);    /* its devices fail; not restarted */
void unit_kill(struct unit *u); /* an unbind: restarted at once unless stopping */
/* Ask it to stop (V6 step 7): killed if it can't be asked, or hasn't by
 * DRV_STOP_MS. Restarted at once, as a kill is, unless hwdb is stopping. */
void unit_stop(struct unit *u);
/* hwdb stops: dos ended, then every unit asked to stop, in reverse graph
 * order. hwdb.finished once none is left. */
void hwdb_stop_all(void);
void unit_retry(struct unit *u);    /* start one given up on, afresh */
void say(const char *text);
void hwdb_report(void);      /* the lines worth the console */
void hwdb_facts(void);       /* what the drivers bound, failed and declined: "hw: " lines */
void say_unit(const struct unit *u, const char *text);
unsigned say_tasks(Handle proc, const char *who);
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
constexpr uint32_t CONN_DISPLAY = UINT32_MAX - 3;   /* and display's, in test mode */
constexpr uint32_t CONN_INPUT = UINT32_MAX - 4;     /* and input's, in test mode */
constexpr uint32_t CONN_INTUITION = UINT32_MAX - 5; /* and intuition's, in test mode */
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
struct unit *power_unit(void);
bool stop_power(struct unit *u);    /* its SCMI channels free, as a kill may not leave them */

/* --- display (display.c): its stop method, and its test. --- */

bool stop_display(struct unit *u);  /* every plane off but the console's, latched */
void display_up(void);              /* its unit is running: connect (test mode) */
void display_down(void);
void display_serve(struct MsgPort *mp);
bool display_native(void);          /* the screen is a controller's, not read back */
bool display_test(void);            /* the kill test's display step: true when it's over */
struct unit *display_unit(void);    /* the console's display, running */
uint32_t display_buffers(void);     /* display's, every client's, as last asked; UINT32_MAX: not known */
bool display_ask(void);             /* ask again; true once the answer is in */

/* --- intuition (intuition.c): the server kept going while it's wanted; the window test. --- */

void intuition_keep(void);          /* started, and again when it ends, while wanted; ended when stopping */
void intuition_desktop(void);       /* the system's, with the Workbench: once display is up, unless "nointuition" */
uint64_t intuition_due(void);       /* when it or the Workbench is to start again; 0 if neither */
void intuition_conn_down(void);
void intuition_conn_serve(struct MsgPort *mp);
int64_t intuition_kill_client(uint64_t conn);   /* HWDB_KILL_CLIENT: its client on 'conn', ended */
int64_t intuition_new_shell(void);  /* HWDB_NEW_SHELL: a conwin started, and its Shell; hwdb's to end */
unsigned intuition_shells(void);    /* conwins running */
int64_t intuition_new_prefs(void);  /* HWDB_NEW_PREFS: Prefs started, one at most; ERR_EXISTS if it's running */
bool window_test(void);             /* the kill test's window step: true when it's over */
bool desktop_test(void);            /* the kill test's desktop step (V8 step 8): true when it's over */
int64_t intuition_test_kill(uint32_t what);     /* HWDB_TEST_KILL: during the desktop step only */

/* --- input (input.c): the server kept going; xhci's stop method; the test. --- */
void input_keep(void);              /* started, and again when it ends; ended when stopping */
uint64_t input_due(void);           /* when it's to start again; 0 if running */
bool stop_xhci(struct unit *u);     /* every controller halted */
void input_conn_down(void);
void input_conn_serve(struct MsgPort *mp);
bool input_test(void);              /* the kill test's input step: true when it's over */
unsigned input_kill(void);          /* killed (it's started again): its starts so far */
unsigned input_starts(void);

/* --- gpu-panthor (gpu.c): its stop method; the test. --- */
bool stop_gpu(struct unit *u);      /* the MCU off and the GPU reset */
bool stop_rogue(struct unit *u);    /* the RV2's GPU: all of it in soft reset */
bool gpu_test(void);                /* the kill test's GPU step: true when it's over */
bool gpu_load_test(void);           /* step 7's: the kills under a GPU client's load */

/* --- pci (pci.c): a host bridge's driver, while it binds. --- */

bool pci_host(const struct unit *u, struct HwdbMsg *m);            /* fills m->hm_Host */
int64_t pci_function(const struct unit *u, const struct HwdbMsg *m);  /* 0, or why not */

/* The kill test (killtest.c): a step each time round the loop, and when
 * it next wants one (0: no time). */
void test_step(void);
uint64_t test_deadline(void);

#endif /* HWDB_SERVER_H */
