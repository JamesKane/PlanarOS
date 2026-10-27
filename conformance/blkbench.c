/*
 * conformance/blkbench.c -- 4 KiB random reads at queue depths 1, 2, 8
 * and 32 (docs §11.8, v4-plan step 5's measurement)
 *
 * hwdb's test mode runs it on each block port before the kill test, the
 * port's unit as its argument ("nvme.N", or with bit 8 "sd.N"). On the test disk it reads the
 * second partition (tools/mkdisk.py: every block holds its own number,
 * which each read is checked against); on any other (a board's own) it
 * reads the whole disk and checks nothing. It never writes. It keeps
 * the queue at its depth with
 * lib/blk's asynchronous calls and heap buffers: the device writes the
 * connection's heap, which this reads, with no copy (VQ3).
 *
 * For each depth: IOPS, latency (mean, median, 99th percentile), and
 * per I/O the kernel calls, interrupts and crossings between domains,
 * from QuerySystem. Those are the whole machine's, so other work shows
 * in them; hwdb is idle while this runs. Exit 0 if every read was right.
 */
#include <planar/blk.h>
#include <planar/endian.h>
#include <planar/syscalls.h>

constexpr int64_t SIG_BLK = 16;
constexpr unsigned DEPTHS[] = { 1, 2, 8, 32 };
constexpr unsigned IOS = 4000;              /* per depth, at most */
constexpr uint64_t RUN_NS = 300000000;      /* per depth, at most */
constexpr uint64_t BYTES = 4096;

static uint64_t lat[IOS];

struct line {
    char text[256];
    unsigned len;
};

static void put(struct line *l, const char *s)
{
    while (*s && l->len < sizeof l->text - 1)
        l->text[l->len++] = *s++;
    l->text[l->len] = '\0';
}

static void put_dec(struct line *l, uint64_t v)
{
    char d[24];
    unsigned n = 0;

    do
        d[n++] = (char)('0' + v % 10);
    while ((v /= 10) && n < sizeof d);
    while (n && l->len < sizeof l->text - 1)
        l->text[l->len++] = d[--n];
    l->text[l->len] = '\0';
}

static void say(const char *s)
{
    KLog(s);
}

static void say_dec(const char *s, uint64_t v)
{
    struct line l = { 0 };

    put(&l, s);
    put_dec(&l, v);
    KLog(l.text);
}
static uint64_t seed = 0x9e3779b97f4a7c15ULL;

static uint64_t rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}

static void sort(uint64_t *v, unsigned n)
{
    for (unsigned gap = n / 2; gap; gap /= 2)
        for (unsigned i = gap; i < n; i++)
            for (unsigned j = i; j >= gap && v[j - gap] > v[j]; j -= gap) {
                uint64_t t = v[j];

                v[j] = v[j - gap];
                v[j - gap] = t;
            }
}

/* Hundredths, as "12.34". */
static void put_frac(struct line *l, uint64_t num, uint64_t den)
{
    uint64_t h = den ? (num * 100 + den / 2) / den : 0;

    put_dec(l, h / 100);
    put(l, ".");
    put_dec(l, h / 10 % 10);
    put_dec(l, h % 10);
}

struct io {
    struct BlkMsg *m;
    uint8_t *buf;
    uint64_t block, at;
};

int main(uint64_t unit)
{
    char port[16] = "nvme.";
    unsigned at = 5;
    struct blk b;
    struct io io[32] = { 0 };
    uint32_t per;
    unsigned wrong = 0;
    bool check = false;
    int64_t e;

    if (unit >> 8 & 1) {                /* "sd.N" */
        port[0] = 's', port[1] = 'd', port[2] = '.';
        at = 3;
    }
    for (unsigned i = at, k = 0, u = (unsigned)(unit & 0xff);; ) {
        char d[8];

        do
            d[k++] = (char)('0' + u % 10);
        while ((u /= 10) && k < sizeof d);
        while (k)
            port[i++] = d[--k];
        port[i] = '\0';
        break;
    }
    if ((e = blk_open(&b, port, SIG_BLK, 1 << 20)) != 0) {
        say_dec("blkbench: no disk to read: error ", (uint64_t)-e);
        return 1;
    }
    /* The test disk's second partition, whose blocks hold their own
     * numbers (tools/mkdisk.py); any other disk is read, never written,
     * across the whole of it, and its blocks can't be checked. */
    if (blk_partition(&b, 2) == 0 && b.block_size <= BYTES) {
        uint8_t *first = blk_alloc(&b, b.block_size);

        check = first && blk_read(&b, 0, 1, first) == 0 && get_le64(first) == b.first;
    }
    if (!check) {
        b.first = 0;
        b.blocks = b.dev_blocks;
    }
    per = (uint32_t)(BYTES / b.block_size);
    if (per == 0 || b.blocks < 2 * per) {
        say("blkbench: the partition is too small");
        return 2;
    }
    for (unsigned i = 0; i < 32; i++)
        if (!(io[i].buf = blk_alloc(&b, BYTES)))
            return 3;

    for (unsigned d = 0; d < sizeof DEPTHS / sizeof DEPTHS[0]; d++) {
        unsigned depth = DEPTHS[d], done = 0, out = 0;
        struct SystemInfo s0, s1;
        uint64_t start, end, sum = 0;
        struct line l = { 0 };

        QuerySystem(&s0);
        start = CurrentTime();
        for (;;) {
            /* Keep 'depth' out, until the run is over. */
            while (out < depth && done + out < IOS && CurrentTime() - start < RUN_NS) {
                unsigned i = 0;

                while (io[i].m)
                    i++;
                io[i].block = (rnd() % (b.blocks / per - 1)) * per;
                io[i].at = CurrentTime();
                if (!(io[i].m = blk_submit(&b, io[i].block, per, io[i].buf))) {
                    say("blkbench: a read couldn't be sent");
                    return 4;
                }
                out++;
            }
            if (out == 0)
                break;
            struct BlkMsg *m = blk_done(&b);
            unsigned i = 0;

            if (!m) {
                say("blkbench: the driver went");
                return 5;
            }
            while (i < 32 && io[i].m != m)
                i++;
            if (i == 32) {
                say("blkbench: a reply for no read it sent");
                return 8;
            }
            if (m->bm_Msg.mn_Error || m->bm_Req[0].br_Error)
                wrong++;
            for (uint32_t k = 0; k < per && check; k++)
                wrong += get_le64(io[i].buf + k * b.block_size) != b.first + io[i].block + k;
            lat[done] = CurrentTime() - io[i].at;
            sum += lat[done];
            done++;
            blk_release(&b, m);
            io[i].m = nullptr;
            out--;
        }
        end = CurrentTime();
        QuerySystem(&s1);
        sort(lat, done);

        put(&l, "blkbench: ");
        put(&l, port);
        put(&l, " 4 KiB random reads at depth ");
        put_dec(&l, depth);
        put(&l, ": ");
        put_dec(&l, done * 1000000000ULL / (end - start));
        put(&l, " IOPS; latency mean ");
        put_dec(&l, sum / done / 1000);
        put(&l, " us, median ");
        put_dec(&l, lat[done / 2] / 1000);
        put(&l, ", 99th ");
        put_dec(&l, lat[done * 99 / 100] / 1000);
        put(&l, "; per I/O ");
        put_frac(&l, s1.si_Calls - s0.si_Calls, done);
        put(&l, " calls, ");
        put_frac(&l, s1.si_Interrupts - s0.si_Interrupts, done);
        put(&l, " interrupts, ");
        put_frac(&l, s1.si_Crossings - s0.si_Crossings, done);
        put(&l, " crossings (");
        put_dec(&l, done);
        put(&l, check ? " reads, each checked)" : " reads, unchecked: not the test disk)");
        KLog(l.text);
    }
    blk_close(&b);
    if (wrong) {
        say_dec("blkbench: reads that came back wrong: ", wrong);
        return 6;
    }
    return 0;
}
