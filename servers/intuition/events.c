/*
 * servers/intuition/events.c -- a client's IDCMP events, kept until it
 * has room (v7-plan step 4)
 */
#include "events.h"

static void drop(struct ie_queue *q, unsigned i)
{
    for (q->n--; i < q->n; i++)
        q->q[i] = q->q[i + 1];
}

void ie_push(struct ie_queue *q, const struct IntuiEvent *e, volatile uint64_t *lost)
{
    bool move = e->iev_Class == IDCMP_MOUSEMOVE;

    if (move && q->n && q->q[q->n - 1].iev_Class == IDCMP_MOUSEMOVE &&
        q->q[q->n - 1].iev_Window == e->iev_Window) {
        q->q[q->n - 1] = *e;
        return;
    }
    if (q->n == IE_KEPT) {
        unsigned i = 0;

        while (i < q->n && q->q[i].iev_Class != IDCMP_MOUSEMOVE)
            i++;
        (*lost)++;
        if (i == q->n)
            return;
        drop(q, i);
    }
    q->q[q->n++] = *e;
}

bool ie_flush(struct ie_queue *q, struct IntuiPage *page, uint64_t tail)
{
    uint64_t head = page->ip_Head, was = head;
    unsigned i = 0;

    if (tail > head)
        tail = head;
    if (head - tail > INTUI_EVENTS_RING)
        tail = head - INTUI_EVENTS_RING;
    for (; i < q->n && head - tail < INTUI_EVENTS_RING; i++, head++) {
        if (q->q[i].iev_Class == IDCMP_MOUSEMOVE && head - tail >= INTUI_EVENTS_MOVES)
            break;                      /* the rest of the ring kept for keys and buttons */
        page->ip_Event[head % INTUI_EVENTS_RING] = q->q[i];
    }
    if (head == was)
        return false;
    for (unsigned k = i; k < q->n; k++)
        q->q[k - i] = q->q[k];
    q->n -= i;
    __atomic_store_n(&page->ip_Head, head, __ATOMIC_RELEASE);
    return true;
}
