/*
 * drivers/sbsa_wdt.h -- the Arm SBSA generic watchdog, from the GTDT
 */
#ifndef DRIVERS_SBSA_WDT_H
#define DRIVERS_SBSA_WDT_H

/* Arm the GTDT's first watchdog and keep refreshing it; false if the
 * board has none. Needs the timers (time_init). */
bool sbsa_wdt_init(void);

#endif /* DRIVERS_SBSA_WDT_H */
