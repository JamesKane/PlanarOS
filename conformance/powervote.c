/*
 * conformance/powervote.c -- a client of power (v5-plan step 2)
 *
 * Started by hwdb's power test (servers/hwdb/power.c), as a GPU server
 * will be: it votes for the set "gpu", checking the refusals on the
 * way, and then waits to be killed. hwdb watches "gpu" come on, kills
 * it, and checks power took its vote back (§11.5: provider state is
 * per connection). It ends by itself only if a check failed, with that
 * check's number.
 */
#include <stddef.h>

#include <planar/devices/power.h>
#include <planar/syscalls.h>

#include "log.h"

static unsigned step;

static int failed(const char *what)
{
    KPrintF("powervote: check %lu failed: %s", (uint64_t)step, what);
    return (int)step;
}

#define CHECK(cond)     do { step++; if (!(cond)) return failed(#cond); } while (0)

static int64_t ask(struct MsgPort *mp, struct PowerMsg *m, uint32_t op, uint32_t tag,
                   const char *name)
{
    memset(m, 0, sizeof *m);
    m->pm_Msg.mn_Length = sizeof *m;
    m->pm_Op = op;
    m->pm_Tag = tag;
    for (unsigned i = 0; name && name[i] && i < POWER_NAME - 1; i++)
        m->pm_Name[i] = name[i];
    if (DoMsg(mp, &m->pm_Msg) != 0)
        return ERR_PORT_DIED;
    return m->pm_Msg.mn_Error;
}

int main(uint64_t)
{
    struct MsgPort mp;
    struct PowerMsg *m;

    CHECK(ConnectPort(&mp, "power", 20, 4, 2 * sizeof *m) == 0);
    CHECK((m = AllocMsg(&mp, sizeof *m)) != nullptr);

    /* Refusals. */
    CHECK(ask(&mp, m, POWER_ON, 1, "no-such-set") == ERR_NOTFOUND);
    CHECK(ask(&mp, m, POWER_OFF, 1, nullptr) == ERR_NOTFOUND);
    CHECK(ask(&mp, m, 99, 1, nullptr) == ERR_BADARG);
    CHECK(ask(&mp, m, POWER_ON, 1, nullptr) == ERR_BADARG);     /* no name, no refs */
    memset(m, 0, sizeof *m);
    m->pm_Msg.mn_Length = sizeof *m;
    m->pm_Op = POWER_ON;
    m->pm_Count = 1;
    m->pm_Ref[0] = (struct PowerRef){ .pr_Kind = POWER_CLOCK, .pr_Device = 99, .pr_NCells = 1 };
    CHECK(DoMsg(&mp, &m->pm_Msg) == 0 && m->pm_Msg.mn_Error == ERR_BADARG);

    /* A vote, a second under the same tag, and one taken back. */
    CHECK(ask(&mp, m, POWER_ON, 2, "gpu") == 0);
    CHECK(ask(&mp, m, POWER_ON, 2, "gpu") == ERR_EXISTS);
    CHECK(ask(&mp, m, POWER_QUERY, 0, "gpu") == 0 && m->pm_On == m->pm_Total && m->pm_Total > 0);
    CHECK(ask(&mp, m, POWER_OFF, 2, nullptr) == 0);
    CHECK(ask(&mp, m, POWER_OFF, 2, nullptr) == ERR_NOTFOUND);

    /* The one hwdb watches: held until it kills us. */
    CHECK(ask(&mp, m, POWER_ON, 1, "gpu") == 0);
    for (;;)
        Wait(1ULL << 40);
}
