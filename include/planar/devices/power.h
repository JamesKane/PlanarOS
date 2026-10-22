/*
 * planar/devices/power.h -- the power class (docs §11.5, v5-plan step 2)
 *
 * The power server publishes one port, "power". A client connects and
 * votes: POWER_ON names a set of resources (power domains, clocks,
 * resets) and a tag of the client's own; the server turns on whatever
 * in it is off (domains first, then clocks, then resets released) and
 * holds it on while the vote stands. POWER_OFF takes a vote back. A
 * resource goes off when its last vote goes, in the reverse order, and
 * only if the server turned it on itself: what the firmware left on
 * stays on, since something nobody votes for may need it.
 *
 * Votes are the connection's: when a client goes, dead or not, every
 * vote it held goes with it (§11.5, "provider state is per connection").
 *
 * A set is either firmware references or a name. hwdb votes for each
 * driver before granting its lease (D3), with the references its nodes
 * make to power's devices: each names which of power's devices provides
 * it (pr_Device, in the order hwdb gave power them) and the specifier's
 * cells, as the firmware wrote them. A name is a set the server knows
 * for a device the firmware doesn't describe well enough, such as
 * "gpu".
 *
 * A reference the server doesn't know (a clock outside the registers
 * it's leased) is left as the firmware set it, and counted in
 * pm_Untouched; it isn't an error.
 */
#ifndef PLANAR_DEVICES_POWER_H
#define PLANAR_DEVICES_POWER_H

#include <stdint.h>

#include <planar/msg.h>

constexpr uint32_t POWER_VERSION = 1;
constexpr unsigned POWER_REFS = 24;
constexpr unsigned POWER_NAME = 24;
constexpr uint64_t POWER_DEADLINE_MS = 2000;   /* a vote's answer, at most (V6 step 7) */

/* PowerMsg.pm_Op */
enum : uint32_t {
    POWER_ON = 1,       /* pm_Tag, then pm_Name or pm_Count refs in; pm_TurnedOn, pm_Untouched out */
    POWER_OFF = 2,      /* pm_Tag in: that vote taken back */
    POWER_QUERY = 3,    /* pm_Name or refs in; pm_Total, pm_On, pm_Kept, pm_Untouched out */
};

/* PowerRef.pr_Kind */
enum : uint32_t { POWER_DOMAIN = 1, POWER_CLOCK = 2, POWER_RESET = 3,
                  POWER_SUPPLY = 4 };   /* a supply's: a set's alone so far, not a reference's */

struct PowerRef {
    uint32_t pr_Kind;
    uint32_t pr_Device;             /* which of the server's devices provides it */
    uint32_t pr_NCells;
    uint32_t pr_Cells[4];
};

struct PowerMsg {
    struct Message pm_Msg;
    uint32_t pm_Op;
    uint32_t pm_Tag;                /* the client's name for the vote */
    uint32_t pm_Count;              /* refs in pm_Ref, when pm_Name is "" */
    uint32_t pm_TurnedOn;           /* POWER_ON: resources it turned on, being off */
    uint32_t pm_Total;              /* POWER_QUERY: resources in the set */
    uint32_t pm_On;                 /* POWER_QUERY: of those, on now (read from the hardware) */
    uint32_t pm_Kept;               /* POWER_QUERY: of those, the firmware's: never turned off */
    uint32_t pm_Untouched;          /* references the server leaves as they are */
    char pm_Name[POWER_NAME];
    struct PowerRef pm_Ref[POWER_REFS];
};

#endif /* PLANAR_DEVICES_POWER_H */
