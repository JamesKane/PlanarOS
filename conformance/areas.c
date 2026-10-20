/*
 * conformance/areas.c -- Areas from user mode
 *
 * Allocates Areas, checks what QueryArea says, changes rights, runs code
 * it wrote, frees them, tries bad arguments, and churns: Areas of
 * different sizes come and go, each keeping its contents and none
 * overlapping another. Exits 0 if every check passed, otherwise with the
 * number of the first that failed. The same in either layout. From
 * omega's conformance/areas.c, less what needs ports (V2).
 */
#include <planar/exec.h>
#include <planar/syscalls.h>

#include "code.h"
#include "log.h"

constexpr uint64_t PAGE = 4096;
constexpr unsigned SLOTS = 16;
constexpr unsigned CHURN = 1500;

static unsigned step;

static int failed(const char *what)
{
    struct logbuf b = { 0 };

    log_str(&b, "areas: check ");
    log_num(&b, step);
    log_str(&b, " failed: ");
    log_str(&b, what);
    KLog(b.text);
    return (int)step;
}

#define CHECK(cond)     do { step++; if (!(cond)) return failed(#cond); } while (0)

static uint64_t rng = 0x2545f4914f6cdd1dULL;

static uint64_t next_random(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

int main(void)
{
    struct AreaInfo info;
    uint8_t *p, *q, *slot_at[SLOTS];
    int64_t a, b, slot[SLOTS];      /* handles, or errors */
    uint64_t v, slot_size[SLOTS];
    bool kept = true, apart = true;

    /* Allocated, zeroed, page-rounded, and described by QueryArea. */
    a = AllocArea(3 * PAGE + 1, AREAF_CLEAR, (void **)&p);
    CHECK((int64_t)a > 0 && p);
    CHECK(QueryArea(p + 5000, &info) == 0);
    CHECK(info.ai_Address == p && info.ai_Size == 4 * PAGE);
    CHECK(info.ai_Rights == (AREAR_READ | AREAR_WRITE) && info.ai_Area == a);
    CHECK(info.ai_Sharers == 1 && info.ai_Flags == AREAF_CLEAR);
    v = 0;
    for (uint64_t i = 0; i < 4 * PAGE; i++)
        v |= p[i];
    CHECK(v == 0);
    for (uint64_t i = 0; i < 4 * PAGE; i++)
        p[i] = (uint8_t)(i * 7);
    v = 0;
    for (uint64_t i = 0; i < 4 * PAGE; i++)
        v |= p[i] ^ (uint8_t)(i * 7);
    CHECK(v == 0);
    CHECK(QueryArea(p + 4 * PAGE, &info) == ERR_NOTFOUND);

    /* Rights: execute needs AREAF_EXEC, write needs read. */
    CHECK(ProtectArea(a, AREAR_READ | AREAR_EXEC) == ERR_RIGHTS);
    CHECK(ProtectArea(a, AREAR_WRITE) == ERR_BADARG);
    CHECK(ProtectArea(a, AREAR_READ) == 0);
    CHECK(QueryArea(p, &info) == 0 && info.ai_Rights == AREAR_READ);
    CHECK(p[3 * PAGE + 5] == (uint8_t)((3 * PAGE + 5) * 7));
    CHECK(ProtectArea(a, AREAR_READ | AREAR_WRITE) == 0);
    p[3 * PAGE + 5] = 1;

    /* Code written into an Area runs once it's made executable, and the
     * Area is never writable and executable at once. */
    b = AllocArea(PAGE, AREAF_EXEC, (void **)&q);
    CHECK((int64_t)b > 0);
    code_return42((uint32_t *)q);
    CHECK(ProtectArea(b, AREAR_READ | AREAR_WRITE | AREAR_EXEC) == ERR_RIGHTS);
    CHECK(ProtectArea(b, AREAR_READ | AREAR_EXEC) == 0);
    CHECK(((int (*)(void))(void *)q)() == 42);
    CHECK(FreeArea(b) == 0);
    b = AllocArea(PAGE, AREAF_RWX, (void **)&q);
    CHECK((int64_t)b > 0 && ProtectArea(b, AREAR_READ | AREAR_WRITE | AREAR_EXEC) == 0);
    CHECK(FreeArea(b) == 0);

    /* Freed, it's gone, and so is its handle. */
    CHECK(FreeArea(a) == 0);
    CHECK(QueryArea(p, &info) == ERR_NOTFOUND);
    CHECK(FreeArea(a) == ERR_BADHANDLE);
    CHECK(ProtectArea(a, AREAR_READ) == ERR_BADHANDLE);

    /* Bad arguments, and the other flags. */
    CHECK(AllocArea(0, 0, (void **)&q) == ERR_BADARG);
    CHECK(AllocArea(PAGE, 1u << 20, (void **)&q) == ERR_BADARG);
    b = AllocArea(PAGE, AREAF_LARGE, (void **)&q);
    CHECK((int64_t)b > 0 && ((uint64_t)(uintptr_t)q & ((2u << 20) - 1)) == 0 && FreeArea(b) == 0);
    b = AllocArea(8 * PAGE, AREAF_CONTIG, (void **)&q);
    CHECK((int64_t)b > 0 && FreeArea(b) == 0);
    CHECK(QueryArea((void *)8, &info) == ERR_NOTFOUND);

    /* More than there is: refused, as an error (review M5's handle-or-
     * error return; the kernel tests failing part way itself). */
    CHECK(AllocArea(31ULL << 30, 0, (void **)&q) == ERR_NOMEM);

    /* Churn: each live Area keeps its contents and no two overlap. */
    for (unsigned i = 0; i < SLOTS; i++)
        slot[i] = 0;
    for (unsigned n = 0; n < CHURN; n++) {
        unsigned i = (unsigned)(next_random() % SLOTS);

        if (slot[i]) {
            for (uint64_t off = 0; off < slot_size[i]; off += 512)
                kept &= slot_at[i][off] == (uint8_t)(i + 1);
            if (FreeArea(slot[i]) != 0)
                return failed("FreeArea in the churn");
            slot[i] = 0;
            continue;
        }
        slot_size[i] = (1 + next_random() % 8) * PAGE;
        slot[i] = AllocArea(slot_size[i], 0, (void **)&slot_at[i]);
        if ((int64_t)slot[i] <= 0)
            return failed("AllocArea in the churn");
        for (unsigned j = 0; j < SLOTS; j++)
            if (j != i && slot[j])
                apart &= slot_at[i] + slot_size[i] <= slot_at[j] ||
                         slot_at[j] + slot_size[j] <= slot_at[i];
        for (uint64_t off = 0; off < slot_size[i]; off += 512)
            slot_at[i][off] = (uint8_t)(i + 1);
    }
    CHECK(kept);
    CHECK(apart);
    for (unsigned i = 0; i < SLOTS; i++)
        if (slot[i])
            FreeArea(slot[i]);

    KLog("areas: every check passed");
    return 0;
}
