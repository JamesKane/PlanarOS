/*
 * servers/conwin/conwin.h -- what conwin's windows are, for the tests
 * that look at them (conformance/window.c) as well as conwin
 */
#ifndef SERVERS_CONWIN_CONWIN_H
#define SERVERS_CONWIN_CONWIN_H

#include <stdint.h>

constexpr unsigned CONWIN_MAX = 8;          /* con.1 to con.8 */
constexpr uint32_t CONWIN_COLS = 80, CONWIN_ROWS = 24;  /* a new window's, as the screen allows */
/* con.n's window's top left (its chrome's): cascaded from con.1's. */
constexpr int32_t CONWIN_LEFT = 60, CONWIN_TOP = 60, CONWIN_STEP = 30;
constexpr uint32_t CONWIN_MIN_COLS = 20, CONWIN_MIN_ROWS = 4;

#endif /* SERVERS_CONWIN_CONWIN_H */
