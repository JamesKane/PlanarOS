/*
 * drv/power/main.c -- the power server (docs §2.4, §11.5; v5-plan step 2)
 *
 * A driver like the others: hwdb starts it on the devices that provide
 * power (the K1's clock and reset controllers and power domains, the
 * CIX's SCMI mailbox and channel, or QEMU's model), and it publishes
 * "power" (planar/devices/power.h). hwdb is its first client: it votes
 * for each driver before granting its lease. core.c keeps the votes;
 * each client's go when its connection does.
 */
#include <stddef.h>

#include <planar/drv.h>
#include <planar/syscalls.h>

#include "power.h"

void *memcpy(void *restrict dst, const void *restrict src, size_t n);

static struct drv *drv;
static bool started;

void pw_say(const char *text)
{
    KLog(text);
}

uint64_t pw_now_us(void)
{
    return CurrentTime() / 1000;
}

/* Short waits spin: the longest is a domain's 300 us. */
void pw_delay_us(uint64_t us)
{
    uint64_t until = CurrentTime() + us * 1000;

    while (CurrentTime() < until)
        ;
}

int64_t pw_firmware_call(uint64_t x[4])
{
    return CallLease(drv_lease(drv), x);
}

volatile uint8_t *pw_map(uint64_t base, uint64_t size)
{
    for (unsigned r = 0; r < drv->info.hm_NRanges; r++) {
        const struct DrvRange *rg = &drv->info.hm_Range[r];

        if (base >= rg->dr_Base && size <= rg->dr_Size && base - rg->dr_Base <= rg->dr_Size - size)
            return (volatile uint8_t *)base;    /* leases map where they are */
    }
    return nullptr;
}

static int bind(struct drv *d, struct drv_dev *dev)
{
    drv = d;
    pw_device(dev->info->dd_Key, dev->info->dd_Size ? dev->regs : nullptr, dev->info->dd_Base,
              dev->info->dd_Size);
    return pw_dev[pw_ndev - 1].be ? DRV_OK : DRV_DECLINE;
}

/* Once every device is in: the backends, then the one port. */
static int start(struct drv *d, struct drv_dev *dev)
{
    if (started)
        return 0;
    started = true;
    if (!pw_start()) {
        drv_log(d, nullptr, "no backend could use its hardware");
        return -1;
    }
    return (int)drv_publish_as(d, dev, "power");
}

static void message(struct drv *d, struct drv_dev *, unsigned c, struct Message *m)
{
    struct PowerMsg copy;

    memcpy(&copy, m, sizeof copy);          /* the client's memory: read once */
    pw_message(c, &copy);
    memcpy(m, &copy, sizeof copy);
    drv_reply(d, c, m);
}

static void gone(struct drv *, struct drv_dev *, unsigned c)
{
    pw_gone(c);
}

static const struct drv_ops ops = {
    .name = "power", .bind = bind, .start = start, .message = message,
    .message_size = sizeof(struct PowerMsg), .gone = gone,
};

int main(uint64_t token)
{
    drv_main(token, &ops);
}
