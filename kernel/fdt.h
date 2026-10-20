/*
 * kernel/fdt.h -- read-only flattened device tree access (docs §9.4, layer 0)
 *
 * The kernel reads the firmware's device tree at boot for what it needs
 * before any server runs: memory, CPUs, the interrupt controller, timers
 * and the console. The full device graph is the hwdb server's job. Nodes
 * are named by their offset in the structure block; negative means none.
 *
 * Carried over from omega's kernel/fdt.c.
 */
#ifndef KERNEL_FDT_H
#define KERNEL_FDT_H

#include <stddef.h>
#include <stdint.h>

/* Validate the blob and make it the tree the other calls read. If it
 * isn't good (nullptr, say), there's no tree: every lookup finds nothing. */
bool fdt_init(const void *blob);

/* Node by absolute path. A component without '@' matches any unit address. */
int32_t fdt_path(const char *path);

/* Node by path of 'len' bytes (not NUL-terminated), or by alias name if it
 * doesn't start with '/', as /chosen's stdout-path may be. */
int32_t fdt_path_or_alias(const char *path, size_t len);

/* Next node after 'after' (-1: from the start) whose compatible list
 * contains 'compat', or whose name before '@' is 'name'. */
int32_t fdt_find_compatible(int32_t after, const char *compat);
int32_t fdt_find_name(int32_t after, const char *name);

/* The next direct child of 'parent' after 'after' (-1: the first one). */
int32_t fdt_child(int32_t parent, int32_t after);

/* The node whose phandle property is 'phandle', and a node's parent. */
int32_t fdt_find_phandle(uint32_t phandle);
int32_t fdt_parent(int32_t node);

/* Does the node's compatible list contain 'compat'? */
bool fdt_is_compatible(int32_t node, const char *compat);

const char *fdt_node_name(int32_t node);
const void *fdt_getprop(int32_t node, const char *name, uint32_t *len);
uint32_t fdt_getprop_u32(int32_t node, const char *name, uint32_t fallback);

/* Entry 'index' of the node's reg property, using its parent's cell sizes. */
bool fdt_reg(int32_t node, uint32_t index, uint64_t *addr, uint64_t *size);

/* Entry 'index' of the blob's memory-reservation block; false past the end. */
bool fdt_memreserve(uint32_t index, uint64_t *addr, uint64_t *size);

/* The blob itself: where it is and how many bytes it takes. */
const void *fdt_blob(void);
uint32_t fdt_size(void);

/* Print the node's properties: all of them, or only those named in the
 * nullptr-terminated list 'only'. */
void fdt_dump_node(int32_t node, const char *const *only);

/* Print the model, memory ranges, reservations and CPU count. */
void fdt_report(void);

/* Print the blob in hex, as "dtb: " lines, for capturing a board's tree
 * from its console (tools/dtb-from-log.py): up to the end of its
 * structure or strings, leaving out the padding after them. */
void fdt_dump_blob(void);

#endif /* KERNEL_FDT_H */
