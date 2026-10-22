/*
 * conformance/uncached.c -- Areas the CPU doesn't cache (AREAF_NC,
 * v5-plan step 1)
 *
 * The argument says whether the kernel found it can map them (arch_has_nc):
 * if not, AREAF_NC must be refused with ERR_NOSYS and nothing else is
 * tried. If so: an NC Area holds what's written to it, says it's NC,
 * can't be executable, and can be shared; a DMA-style ring of them is
 * written and read back. Exits 0 if every check passed, otherwise with
 * the number of the first that failed.
 */
#include <planar/exec.h>
#include <planar/syscalls.h>

#include "log.h"

constexpr uint64_t PAGE = 4096;
constexpr uint64_t SIZE = 16 * PAGE;

static unsigned step;

static int failed(const char *what)
{
    KPrintF("uncached: check %lu failed: %s", (uint64_t)step, what);
    return (int)step;
}

#define CHECK(cond)     do { step++; if (!(cond)) return failed(#cond); } while (0)

int main(uint64_t arg)
{
    struct AreaInfo ai;
    volatile uint64_t *w;
    void *at;
    int64_t h;

    if (!arg) {
        CHECK(AllocArea(PAGE, AREAF_NC, &at) == ERR_NOSYS);
        return 0;
    }
    CHECK(AllocArea(PAGE, AREAF_NC | AREAF_EXEC, &at) == ERR_BADARG);
    CHECK(AllocArea(PAGE, AREAF_NC | AREAF_RWX, &at) == ERR_BADARG);
    h = AllocArea(SIZE, AREAF_NC | AREAF_DMA32, &at);
    CHECK(h > 0);
    CHECK((uint64_t)at + SIZE <= 1ULL << 32);
    CHECK(QueryArea(at, &ai) == 0 && (ai.ai_Flags & AREAF_NC) && ai.ai_Size == SIZE);
    CHECK(ProtectArea((Handle)h, AREAR_READ | AREAR_EXEC) == ERR_RIGHTS);
    w = at;
    for (uint64_t i = 0; i < SIZE / 8; i++)
        CHECK(w[i] == 0);                       /* the zeroes reached memory */
    for (uint64_t i = 0; i < SIZE / 8; i++)
        w[i] = i * 0x9e3779b97f4a7c15ULL;
    for (uint64_t i = 0; i < SIZE / 8; i++)
        CHECK(w[i] == i * 0x9e3779b97f4a7c15ULL);
    CHECK(ProtectArea((Handle)h, AREAR_READ) == 0);
    CHECK(w[1] == 0x9e3779b97f4a7c15ULL);
    CHECK(FreeArea((Handle)h) == 0);
    return 0;
}
