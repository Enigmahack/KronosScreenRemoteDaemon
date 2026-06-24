/*
 * kronos_compat.h - Force-included before all headers.
 *
 * The Kronos kernel was built with CONFIG_TRACEPOINTS and CONFIG_CONSTRUCTORS,
 * but does not export __tracepoint_kmalloc. We need these configs defined for
 * correct struct module layout, but must stub out the actual trace calls.
 */
#ifndef _KRONOS_COMPAT_H
#define _KRONOS_COMPAT_H

/*
 * Suppress CONFIG_TRACEPOINTS so inline kmalloc in slab_def.h does not
 * reference __tracepoint_kmalloc (absent from the Kronos kernel exports).
 * This makes struct module 0x10 bytes smaller than the running kernel's,
 * but the module loader zero-fills the difference. The init/cleanup
 * relocations are patched to the correct offsets by patch_module.py.
 */
#undef CONFIG_TRACEPOINTS

/* CONFIG_CONSTRUCTORS adds fields at the END of struct module.
 * The running kernel was built with it, so enable it for correct layout
 * of fields after cleanup_module. */
#ifndef CONFIG_CONSTRUCTORS
#define CONFIG_CONSTRUCTORS 1
#endif

#endif /* _KRONOS_COMPAT_H */
