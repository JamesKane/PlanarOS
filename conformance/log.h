/*
 * conformance/log.h -- KLog, formatted
 *
 * debug.lib's KPrintF: RawDoFmt's conversions, through utility.library's
 * SNPrintf into a line on the stack, cut at 160 bytes, then KLog: its
 * result (ERR_FULL for a line dropped over the rate).
 */
#ifndef CONFORMANCE_LOG_H
#define CONFORMANCE_LOG_H

#include <planar/exec.h>
#include <planar/utility.h>

[[gnu::format(printf, 1, 2)]]
static inline int64_t KPrintF(const char *fmt, ...)
{
    char line[160];
    va_list ap;

    va_start(ap, fmt);
    VSNPrintf(line, sizeof line, fmt, ap);
    va_end(ap);
    return KLog(line);
}

#endif /* CONFORMANCE_LOG_H */
