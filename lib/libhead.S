/*
 * lib/libhead.S -- a shared library's header (planar/libimage.h)
 *
 * Offsets from the image's start, which is at 0 (lib.ld): the kernel
 * reads them before it relocates anything. Each library defines
 * _library_image, its LibraryImage.
 */

    .section .text.head, "a"
    .globl  _start
_start:
    .ascii  "PLIB"
    .4byte  2                       /* LIBIMAGE_FORMAT */
    .8byte  _end - _start
    .8byte  __rela_start - _start
    .8byte  __rela_end - _start
    .8byte  _library_image - _start
    .8byte  __planar_unwind - _start   /* the unwind table (docs §19.4) */
    .8byte  __planar_unwind_end - _start
    .8byte  __build_id - _start        /* the build-id note */
    .8byte  __build_id_end - _start
