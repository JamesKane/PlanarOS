/*
 * servers/intuition/events.h -- a client's IDCMP events, kept until it
 * has room (v7-plan step 4)
 *
 * intuition writes an event to a connection's ring (its IntuiPage) only
 * when the client has read far enough, which it says in its IntuiAck.
 * Until then the event waits here, IE_KEPT at most. Moves fill no more
 * than INTUI_EVENTS_MOVES of the ring, the rest of it kept for keys and
 * buttons, and a move after a move of the same window takes its place
 * here, as only where the pointer is now matters. Past IE_KEPT the
 * oldest move kept is dropped, and if none is, the event that came: so a
 * key or a button is lost only when a client is IE_KEPT +
 * INTUI_EVENTS_RING - INTUI_EVENTS_MOVES (111) of them behind. Each one
 * dropped counts in ip_Lost. Plain C, tested on the host by test.c.
 */
#ifndef SERVERS_INTUITION_EVENTS_H
#define SERVERS_INTUITION_EVENTS_H

#include <stdint.h>

#include <planar/intuition.h>

constexpr unsigned IE_KEPT = 64;

struct ie_queue {
    unsigned n;
    struct IntuiEvent q[IE_KEPT];       /* the oldest first */
};

/* 'e', after those kept; 'lost' counts what's dropped. */
void ie_push(struct ie_queue *q, const struct IntuiEvent *e, volatile uint64_t *lost);

/* What 'page' has room for, given that the client has read 'tail'
 * events, written to its ring. True if any were: the caller wakes
 * ip_Head. A tail the client got wrong is taken as the nearest it could
 * be. */
bool ie_flush(struct ie_queue *q, struct IntuiPage *page, uint64_t tail);

#endif /* SERVERS_INTUITION_EVENTS_H */
