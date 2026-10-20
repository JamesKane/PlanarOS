/*
 * drv/rtc-goldfish/main.c -- the Goldfish real-time clock (Android's
 * emulator, and QEMU RISC-V's virt machine)
 *
 * Nanoseconds since 1970 in two registers: reading TIME_LOW latches
 * TIME_HIGH. A read answers the seconds, as 8 bytes. Its alarm line is
 * bound but not used (V4 step 3).
 */
#include <planar/drv.h>
#include <planar/syscalls.h>

enum : uint32_t { TIME_LOW = 0x00, TIME_HIGH = 0x04 };

constexpr uint64_t NS = 1000000000;
constexpr uint64_t Y2020 = 1577836800;     /* 2020-01-01, in seconds */

static uint64_t now_ns(const struct drv_dev *dev)
{
    uint32_t lo = *(volatile uint32_t *)(dev->regs + TIME_LOW);

    return (uint64_t)*(volatile uint32_t *)(dev->regs + TIME_HIGH) << 32 | lo;
}

/* No ID register: a clock that reads a time after 2020 is one. */
static int bind(struct drv *, struct drv_dev *dev)
{
    return now_ns(dev) / NS > Y2020 ? DRV_OK : DRV_DECLINE;
}

static int start(struct drv *d, struct drv_dev *dev)
{
    return (int)drv_publish(d, dev, "rtc");
}

static void request(struct drv *, struct drv_dev *dev, struct DrvRequest *r)
{
    uint64_t s = now_ns(dev) / NS;

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
    .name = "rtc-goldfish", .bind = bind, .start = start, .request = request,
};

int main(uint64_t token)
{
    drv_main(token, &ops);
}
