/*
 * conformance/shellwin.h -- a Shell in a window, as conformance/window.c
 * types at it on QEMU and servers/conwin/test.c draws it on the host
 * (V8 step 5)
 *
 * The system menu's Shell opens con.1's window (servers/conwin), 80 by
 * 24 cells of System Mono, at CONWIN_LEFT, CONWIN_TOP. Its Shell says
 * the banner and its prompt (sw_crc_ready: the inside so, its CRC as
 * DISPLAY_READ takes it). Then sw_typed is typed and Return: a second
 * Shell runs cmd/scripts/window, a line at a time after the prompt (it's
 * number 2 as well, on the same console), each line's output after it,
 * then the first's prompt again (sw_crc_done). The host test reads the
 * script and holds it to sw_script's lines, and draws both CRCs.
 */
#ifndef CONFORMANCE_SHELLWIN_H
#define CONFORMANCE_SHELLWIN_H

#include <stdint.h>

static const char sw_typed[] = "Shell FROM S:window";
static const char sw_banner[] = "Planar Shell. Type Help for help.\n";
static const char sw_prompt[] = "\x1b[32m2.SYS:>\x1b[0m ";

/* cmd/scripts/window's lines, and what each writes. */
static const char *const sw_script[][2] = {
    { "; A Shell in a window (V8 step 5): conformance/window.c types Shell FROM S:window", "" },
    { "Echo \"Planar's Shell, in a window, in System Mono.\"", "Planar's Shell, in a window, in System Mono.\n" },
    { "Type S:greeting", "Hello from a file on the disk, typed by C:type.\n" },
    { "Echo \"Box drawing ┌─┬─┐ ░▒▓█, Latin-1 café, and 0O 1lI\"",
      "Box drawing ┌─┬─┐ ░▒▓█, Latin-1 café, and 0O 1lI\n" },
};

/* The inside: 80 by 24 cells of 8 by 18 px, and 4 px around them. */
constexpr uint32_t SW_WIDTH = 648, SW_HEIGHT = 440;

constexpr uint32_t sw_crc_ready = 0x16a840ab;
constexpr uint32_t sw_crc_done = 0xc5867f5b;

#endif /* CONFORMANCE_SHELLWIN_H */
