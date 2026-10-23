/* The layout the converter bakes for the mod's hooks, in one place.
 *
 * The converter (scripts/aot-convert.py) writes, at each hooked site, a branch to a
 * dispatch stub in the library's own stub segment. The shim (scripts/bionic-shim.c)
 * finds that stub by decoding the branch at the site, and the stub then tells it where
 * the rest is. Both read this file, so the numbers cannot drift apart: the converter
 * parses it, the shim includes it.
 *
 * The stub, and what the shim reads from it:
 *
 *   +0   adrp x16, <the page the slot is on>
 *   +4   ldr  x17, [x16, #<the slot's offset from that page>]
 *   +8   cbz  x17, #+8         no replacement yet: run the original code instead
 *   +12  br  x17               the replacement
 *   +16  b   <the trampoline>  where the original code is
 *   +20  the slot's distance from this stub, as a signed 64-bit number
 *   +28  the trampoline's distance from this stub, likewise
 *   +36  the magic
 *
 * The two distances are there because the slot and the trampoline are somewhere else in
 * the same image, and the image slides as a whole: a distance between two of its
 * addresses is the same before the slide as after it.
 *
 * The trampoline that follows the stub holds the instruction the hook displaced from the
 * site -- the site's own first instruction -- and a branch back to the instruction after
 * it, so that a caller who wants the original function gets it.
 */
#ifndef HOOK_LAYOUT_H
#define HOOK_LAYOUT_H

#define HOOK_STUB_BYTES 40        /* the dispatch stub and the three numbers after it */
#define HOOK_SLOT_DELTA 20        /* where the stub carries the slot's distance */
#define HOOK_TRAMP_DELTA 28       /* ... and the trampoline's */
#define HOOK_MAGIC_AT 36          /* ... and the magic */
#define HOOK_MAGIC 0x484F4B21u    /* "HOK!" -- tells the shim a stub is one of ours */
#define HOOK_SLOT_BYTES 8         /* the replacement's address, in the library's data */
#define HOOK_TRAMP_MAX 32         /* room for the trampoline of the worst case */

#endif
