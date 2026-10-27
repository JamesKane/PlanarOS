/*
 * planar/devices/host.h -- the host channel (host-channel plan step 2)
 *
 * A line to whatever runs the machine: on QEMU, a virtio-console that
 * tools/boot-check.py serves on a socket. drv/virtio-mmio publishes it
 * as "host.0". It isn't a console: nothing logs through it and no Shell
 * reads it. Tests ask the host for input on it (conformance/hostask.h),
 * where they asked on the log before; a board has none, and a test knows
 * from ConnectPort's ERR_NOTFOUND that nobody's there to ask.
 *
 * HOST_WRITE sends hm_Len bytes of hm_Text and a newline, and is answered
 * once the device has taken them; with hm_Len 0 it sends nothing, and is
 * answered at once (the kill test's client). HOST_READ is answered with
 * the next whole line the host sends, without its newline, in hm_Text
 * and hm_Len: lines that come with no read waiting are kept, HOST_LINES
 * of them, the oldest given first. A line longer than HOST_LINE is cut.
 * One read at a time on a connection: a second is ERR_BUSY.
 */
#ifndef PLANAR_DEVICES_HOST_H
#define PLANAR_DEVICES_HOST_H

#include <stdint.h>

#include <planar/msg.h>

constexpr uint32_t HOST_LINE = 240;         /* a line's bytes, without its newline */
constexpr unsigned HOST_LINES = 8;          /* kept for a read to come */

enum : uint32_t {
    HOST_WRITE = 1,
    HOST_READ = 2,
};

struct HostMsg {
    struct Message hm_Msg;
    uint32_t hm_Op;
    uint32_t hm_Len;
    char hm_Text[HOST_LINE];
};

#endif /* PLANAR_DEVICES_HOST_H */
