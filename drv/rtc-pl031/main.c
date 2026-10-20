/*
 * drv/rtc-pl031/main.c -- Arm's PrimeCell real-time clock (PL031 TRM)
 *
 * QEMU AArch64's RTC. Bind checks the PrimeCell ID; a read answers the
 * seconds counter (RTCDR), as 8 bytes. Its alarm line is bound but not
 * used: the clock is here to prove binding and restart (V4 step 3).
 */
#include <planar/drv.h>
#include <planar/syscalls.h>

enum : uint32_t { RTCDR = 0x000, PERIPHID0 = 0xfe0, PCELLID0 = 0xff0 };

static uint32_t rd(const struct drv_dev *dev, uint32_t off)
{
    return *(volatile uint32_t *)(dev->regs + off);
}

static int bind(struct drv *, struct drv_dev *dev)
{
    uint32_t part = (rd(dev, PERIPHID0) & 0xff) | (rd(dev, PERIPHID0 + 4) & 0xf) << 8;
    uint32_t cell = 0;

    if (dev->info->dd_Size < 0x1000)
        return DRV_DECLINE;
    for (unsigned i = 0; i < 4; i++)
        cell |= (rd(dev, PCELLID0 + 4 * i) & 0xff) << (8 * i);
    return part == 0x031 && cell == 0xb105f00d ? DRV_OK : DRV_DECLINE;
}

static int start(struct drv *d, struct drv_dev *dev)
{
    return (int)drv_publish(d, dev, "rtc");
}

static void request(struct drv *, struct drv_dev *dev, struct DrvRequest *r)
{
    uint64_t s = rd(dev, RTCDR);

    if (r->dr_Op != DRV_READ || r->dr_Len < 8) {
        r->dr_Msg.mn_Error = (int32_t)ERR_BADARG;
        return;
    }
    for (unsigned i = 0; i < 8; i++)
        r->dr_Data[i] = (uint8_t)(s >> (8 * i));
    r->dr_Len = 8;
    r->dr_Msg.mn_Error = 0;
}

static const struct drv_ops ops = {
    .name = "rtc-pl031", .bind = bind, .start = start, .request = request,
};

int main(uint64_t token)
{
    drv_main(token, &ops);
}
