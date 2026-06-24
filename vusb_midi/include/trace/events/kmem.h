/*
 * Stub: the Kronos kernel was built with CONFIG_TRACEPOINTS but does not
 * export __tracepoint_kmalloc / __tracepoint_kmalloc_node.
 * We need CONFIG_TRACEPOINTS defined so struct module has the correct
 * layout, but we must prevent actual tracepoint references in our code.
 * Override the real header with no-op trace macros.
 */
#ifndef _TRACE_KMEM_H_STUB
#define _TRACE_KMEM_H_STUB

#include <linux/tracepoint.h>

static inline void trace_kmalloc(unsigned long call_site, const void *ptr,
				 size_t bytes_req, size_t bytes_alloc,
				 gfp_t gfp_flags) {}
static inline void trace_kmalloc_node(unsigned long call_site, const void *ptr,
				      size_t bytes_req, size_t bytes_alloc,
				      gfp_t gfp_flags, int node) {}
static inline void trace_kfree(unsigned long call_site, const void *ptr) {}
static inline void trace_kmem_cache_alloc(unsigned long call_site, const void *ptr,
					  size_t bytes_req, size_t bytes_alloc,
					  gfp_t gfp_flags) {}
static inline void trace_kmem_cache_alloc_node(unsigned long call_site, const void *ptr,
					       size_t bytes_req, size_t bytes_alloc,
					       gfp_t gfp_flags, int node) {}
static inline void trace_kmem_cache_free(unsigned long call_site, const void *ptr) {}

#endif
