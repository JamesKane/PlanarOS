/*
 * tools/font-oracle/oracle.h -- what the oracle's passes share
 */
#ifndef FONT_ORACLE_H
#define FONT_ORACLE_H

#include <stdio.h>

#include "sfnt.h"

extern unsigned failures;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            if (failures++ < 40) {                                           \
                fprintf(stderr, "font-oracle: %s:%d: ", __FILE__, __LINE__); \
                fprintf(stderr, __VA_ARGS__);                                \
                fputc('\n', stderr);                                         \
            }                                                                \
        }                                                                    \
    } while (0)

/* shaping.c: pass 2 over one font, against HarfBuzz; and the checks of
 * what HarfBuzz can't say (FitLine, controls, UTF-8, the stubs). */
void shaping(const char *font, const uint8_t *buf, size_t len, const struct FontFace *face);
void shaping_alone(void);

#endif /* FONT_ORACLE_H */
