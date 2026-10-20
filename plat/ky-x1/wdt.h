/*
 * plat/ky-x1/wdt.h -- the K1's SoC watchdog (private to plat/ky-x1)
 */
#ifndef PLAT_KY_X1_WDT_H
#define PLAT_KY_X1_WDT_H

/* Take over the watchdog U-Boot left running, and service it. */
void k1_wdt_init(void);

/* Reset the board through it; returns only if that didn't work. */
void k1_wdt_reset(void);

#endif /* PLAT_KY_X1_WDT_H */
