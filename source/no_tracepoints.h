/* Suppress CONFIG_TRACEPOINTS so inline kmalloc in slab_def.h does not
 * reference __tracepoint_kmalloc — absent from the stripped Kronos kernel. */
#undef CONFIG_TRACEPOINTS
