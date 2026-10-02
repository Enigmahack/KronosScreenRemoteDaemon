/*
 * oa_safe.h - shared fault-safe accessors for OA-owned kernel memory.
 *
 * nks4_inject.ko and midi_bridge.ko both read/write memory whose lifetime
 * they don't control (OA.ko's .text, codec ring-buffer control structures
 * that OA can free/move during normal operation). A raw dereference of a
 * since-freed address causes an oops. probe_kernel_read/write route every
 * such touch through the kernel's fault exception tables, turning that into
 * a clean failure instead. See NonCodeTechnicalInfo.md §1 for details.
 *
 * kptr_ok() only rejects obviously-garbage pointers (null, misaligned, out
 * of kernel range) - it cannot tell a plausible pointer from one that's
 * since gone stale, which is exactly why every actual dereference below
 * still goes through probe_kernel_read/write rather than trusting kptr_ok()
 * alone.
 *
 * Originally two copies (nks4_inject.c/midi_bridge.c), merged into this
 * shared header so fixes land in both modules. Each module uses its own
 * call-site macro names (oa_read8/tap_read8) over these functions.
 */
#ifndef OA_SAFE_H
#define OA_SAFE_H

#include <linux/uaccess.h>
#include <linux/types.h>

static inline int kptr_ok(unsigned long p)
{
    return p >= 0x40000000UL && p < 0xfffff000UL && (p & 3) == 0;
}

static inline int oa_probe_read8(unsigned long addr, uint8_t *out)
{
    return probe_kernel_read(out, (void *)addr, sizeof(*out)) == 0;
}

static inline int oa_probe_read32(unsigned long addr, uint32_t *out)
{
    return probe_kernel_read(out, (void *)addr, sizeof(*out)) == 0;
}

/* Block form, for the setup-time byte-pattern scans over OA .text/.bss and
 * for the ring-drain byte copies. Same fault class as the fixed-width reads
 * above - no reason for a variable-length read to be any less safe. */
static inline int oa_probe_readn(unsigned long addr, void *out, size_t n)
{
    return probe_kernel_read(out, (void *)addr, n) == 0;
}

static inline int oa_probe_write8(unsigned long addr, uint8_t v)
{
    return probe_kernel_write((void *)addr, &v, sizeof(v)) == 0;
}

/* Read a pointer out of a .bss singleton slot. Returns NULL if the slot
 * address itself is implausible, if the slot is unmapped, or if the value
 * read back is not a plausible kernel pointer - one NULL check for all
 * three failure modes instead of three separate ones at every call site. */
static inline void *oa_probe_read_ptr(unsigned long slot)
{
    unsigned long v;

    if (!kptr_ok(slot))
        return NULL;
    if (probe_kernel_read(&v, (void *)slot, sizeof(v)) != 0)
        return NULL;
    return kptr_ok(v) ? (void *)v : NULL;
}

#endif /* OA_SAFE_H */
