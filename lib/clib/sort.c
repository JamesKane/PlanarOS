/*
 * lib/clib/sort.c -- qsort and bsearch (docs §17.4.5)
 *
 * qsort is a heapsort: n log n at worst, in place, with no recursion,
 * so a task's small stack and an adversary's input are both safe. It
 * isn't stable, which C doesn't ask of it.
 */
#include <stdint.h>

#include <stdlib.h>

typedef int compare(const void *, const void *);

static void swap(uint8_t *a, uint8_t *b, size_t size)
{
    while (size--) {
        uint8_t t = *a;

        *a++ = *b;
        *b++ = t;
    }
}

/* Moves the element at 'root' down until the heap of n below it holds. */
static void sift(uint8_t *base, size_t root, size_t n, size_t size, compare *compar)
{
    for (size_t child; (child = 2 * root + 1) < n; root = child) {
        if (child + 1 < n && compar(base + child * size, base + (child + 1) * size) < 0)
            child++;
        if (compar(base + root * size, base + child * size) >= 0)
            return;
        swap(base + root * size, base + child * size, size);
    }
}

void qsort(void *base, size_t n, size_t size, compare *compar)
{
    uint8_t *b = base;

    if (n < 2 || !size)
        return;
    for (size_t i = n / 2; i-- > 0;)
        sift(b, i, n, size, compar);
    for (size_t end = n - 1; end > 0; end--) {
        swap(b, b + end * size, size);
        sift(b, 0, end, size, compar);
    }
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, compare *compar)
{
    const uint8_t *lo = base;

    while (n) {
        const uint8_t *mid = lo + (n / 2) * size;
        int c = compar(key, mid);

        if (!c)
            return (void *)mid;
        if (c > 0) {
            lo = mid + size;
            n -= n / 2 + 1;
        } else {
            n /= 2;
        }
    }
    return nullptr;
}
