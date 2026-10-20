/*
 * kernel/userimg.h -- the built-in user programs
 *
 * The build embeds each program under conformance/ as a flat image
 * (mk/user.mk generates the table); the hosted build has none.
 */
#ifndef KERNEL_USERIMG_H
#define KERNEL_USERIMG_H

#include <stdint.h>

struct user_image {
    const char *name;
    const uint8_t *start, *end;
};

/* Ends with an entry whose name is nullptr. */
extern const struct user_image user_images[];

#endif /* KERNEL_USERIMG_H */
