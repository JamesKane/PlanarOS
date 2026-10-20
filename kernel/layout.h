/*
 * kernel/layout.h -- where user memory lives (docs §3.1, VQ3)
 *
 * Identity: an Area is one physically contiguous run, mapped at VA = PA;
 * pointers are physical, so devices see them unchanged, but every Area
 * needs a run of free pages as long as itself, and a stack's guard is a
 * page of memory left unmapped. Global: an Area is a range of a window
 * of the address space (kernel/va.c) backed by pages from anywhere, with
 * guard pages that cost no memory.
 *
 * Chosen at boot by "layout=identity" or "layout=global" in the kernel's
 * arguments (/chosen/bootargs); identity if there's none. Nothing above
 * kernel/area.c depends on which.
 */
#ifndef KERNEL_LAYOUT_H
#define KERNEL_LAYOUT_H

enum layout : unsigned char {
    LAYOUT_IDENTITY,
    LAYOUT_GLOBAL,
};

/* Read the choice from the kernel arguments. */
void layout_init(void);

/* The layout processes get; the soak test can run either. */
enum layout layout_boot(void);
const char *layout_name(enum layout l);

#endif /* KERNEL_LAYOUT_H */
