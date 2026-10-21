/*
 * servers/acpid/acpid.h -- acpid's parts (docs §9.4, v4-plan step 4)
 *
 * acpid runs ACPICA (third_party/acpica, unchanged) in its own process
 * and describes the namespace's devices to hwdb (planar/acpid.h). Two
 * parts are the same on Planar and on the host:
 *
 * - osl.c, ACPICA's OS layer (acpiosxf.h). acpid is one task, so a lock
 *   is never held against it and a semaphore never waits.
 * - walk.c, which loads the tables, runs the _INI methods, and walks the
 *   namespace, describing each device as an AcpidDevice. Operation
 *   regions in memory go through its own handler, which asks the
 *   platform for each window.
 *
 * The platform is the rest: main.c on Planar (hwdb's protocol, the heap,
 * time), host.c on the host (tables from files, for the CIX's count and
 * hwdb's tests). This header names what it gives, without ACPICA's.
 */
#ifndef ACPID_H
#define ACPID_H

#include <stddef.h>
#include <stdint.h>

#include <planar/acpid.h>

/* ACPICA's own (utprint.c; its size is ACPI_SIZE, the same 64 bits),
 * or the host's C library's. */
int snprintf(char *restrict s, size_t n, const char *restrict format, ...);

/* --- The platform's. --- */

void *acpid_alloc(size_t n);            /* zeroed; nullptr when there's no room */
void acpid_free(void *p);
uint64_t acpid_rsdp(void);              /* physical */
uint64_t acpid_now(void);               /* nanoseconds, from any start */
void acpid_sleep(uint64_t ns);
void acpid_say(const char *line);       /* a line for the console */
void acpid_say_acpica(const char *line);    /* one of ACPICA's own messages */

/* Where physical 'pa' (len bytes, within one window) can be read and
 * written: the firmware's tables, or a window hwdb leased. nullptr if
 * hwdb refused it; *refused is then set. */
volatile void *acpid_map(uint64_t pa, uint64_t len, bool *refused);

/* --- walk.c's. --- */

/* Loads the tables and the namespace and runs _INI; false, having said
 * why, if ACPICA wouldn't. */
bool acpid_load(void);

/* Calls 'found' for each device in the namespace, in its order (a
 * parent before its children); the count found. */
typedef void acpid_found(const struct AcpidDevice *d, void *ctx);
unsigned acpid_walk(acpid_found *found, void *ctx);

/* What the walk saw (§9.6's count): devices, those whose description
 * took a method, by what, and those with power methods. */
struct acpid_count {
    unsigned devices, present, described_by_method, power, regions, refused, failed;
    unsigned by[11];                    /* each ACPID_M_ bit */
};
extern struct acpid_count acpid_count;

/* The GIC distributor's registers, from the MADT, or 0 (the probe's). */
uint64_t acpid_gic(void);

/* The count, as lines for the console. */
void acpid_say_count(void);

/* A string's offset in d->ad_Strings (added if new), or 0 if full. */
uint16_t acpid_string(struct AcpidDevice *d, const char *s, size_t n);

#endif /* ACPID_H */
