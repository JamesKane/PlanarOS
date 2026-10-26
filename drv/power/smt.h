/*
 * drv/power/smt.h -- SCMI's shared-memory channel, the SMT layout (SCMI
 * 3.2 §5.1; V7 step 6)
 *
 * Here for scmi.c and for hwdb's stop method (servers/hwdb/power.c),
 * which frees a channel a dead power left busy. An agent writes its
 * message, marks the channel busy (status 0), then rings the platform,
 * which answers and marks it free. A power killed between the two leaves
 * the channel busy and never rung, and every call after it waits for a
 * free that doesn't come: so too, after a warm reset, the firmware's own
 * use of it.
 */
#ifndef DRV_POWER_SMT_H
#define DRV_POWER_SMT_H

#include <stdint.h>

enum : uint32_t { SMT_STATUS = 0x04, SMT_FLAGS = 0x10, SMT_LENGTH = 0x14, SMT_HEADER = 0x18,
                  SMT_PAYLOAD = 0x1c, SMT_SIZE = 0x80 };
constexpr uint32_t SMT_FREE = 1u << 0, SMT_ERROR = 1u << 1;

/*
 * A dead agent's channel made free again. One rung by an SMC was
 * answered before the call returned, so busy means it was never rung:
 * free it now. One rung by a doorbell may still be answered: wait for
 * that until now() passes 'until', then free it. True if it was busy
 * and never answered.
 */
static inline bool smt_release(volatile uint8_t *shm, bool by_smc, uint64_t (*now)(void),
                               uint64_t until)
{
    volatile uint32_t *st = (volatile uint32_t *)(shm + SMT_STATUS);

    while (!by_smc && !(*st & SMT_FREE) && now() <= until)
        ;
    if (*st & SMT_FREE)
        return false;
    *st = SMT_FREE;
    return true;
}

#endif /* DRV_POWER_SMT_H */
