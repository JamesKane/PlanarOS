/*
 * servers/acpid/acplanar.h -- ACPICA's environment in acpid
 *
 * Force-included before every ACPICA file and every acpid file that uses
 * ACPICA's headers, on Planar and on the host (mk/acpica.mk).
 * platform/acenv.h knows a list of systems and stops with #error at any
 * other, so this header takes acenv.h's include guard and defines what
 * acenv.h and a platform header would have. It's VectraOS-Odin's
 * acvectra.h, which ran the same ACPICA in user mode.
 *
 * acpid is one task, so locks and semaphores never wait (osl.c). The C
 * library is the system's (ACPI_USE_SYSTEM_CLIBRARY): on the host, the
 * host's; on Planar, lib/clib (V6 step 5). ACPICA's
 * object caches are its own (ACPI_USE_LOCAL_CACHE). No debugger, no
 * disassembler.
 */
#ifndef __ACENV_H__
#define __ACENV_H__

#define ACPI_BINARY_SEMAPHORE 0
#define ACPI_OSL_MUTEX 1
#define DEBUGGER_SINGLE_THREADED 0
#define DEBUGGER_MULTI_THREADED 1
#define ACPI_SRC_OS_LF_ONLY 0

#include "acgcc.h" /* ACPICA's own for GCC and clang: va_list, inline, flexible arrays */

#define ACPI_MACHINE_WIDTH 64
#define COMPILER_DEPENDENT_INT64 long long
#define COMPILER_DEPENDENT_UINT64 unsigned long long
#define ACPI_USE_SYSTEM_CLIBRARY
#define ACPI_USE_LOCAL_CACHE
#define ACPI_USE_DO_WHILE_0
#define ACPI_MUTEX_TYPE ACPI_BINARY_SEMAPHORE
#define DEBUGGER_THREADING DEBUGGER_SINGLE_THREADED
/* RISC-V may trap a misaligned load; AArch64's device memory does. */
#define ACPI_MISALIGNMENT_NOT_SUPPORTED

/* Hardware-reduced ACPI on both ISAs: no FACS is needed, and no global
 * lock is shared with firmware, so it's always taken at once. */
#define ACPI_ACQUIRE_GLOBAL_LOCK(GLptr, Acquired) Acquired = 1
#define ACPI_RELEASE_GLOBAL_LOCK(GLptr, Pending) Pending = 0
#define ACPI_SEMAPHORE_NULL NULL
#define ACPI_FLUSH_CPU_CACHE()
#define ACPI_STRUCT_INIT(field, value) value
#define ACPI_SYSTEM_XFACE
#define ACPI_EXTERNAL_XFACE
#define ACPI_INTERNAL_XFACE
#define ACPI_INTERNAL_VAR_XFACE
#define ACPI_INIT_FUNCTION

#define ACPI_FILE void *
#define ACPI_FILE_OUT NULL
#define ACPI_FILE_ERR NULL

/* The C library ACPICA calls: on Planar, lib/clib's (include/clib);
 * on the host, the host's. ACPICA has its own snprintf (utprint.c). */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#endif /* __ACENV_H__ */
