#!/usr/bin/env python3
"""
patch_module.py - Patch init_module and cleanup_module offsets for Kronos kernel.

The Kronos kernel's struct module has different offsets than what GCC 12/13
produces with vanilla 2.6.32 headers:
  init_module:    0xbc -> 0xd4   (shift +0x18)
  cleanup_module: 0x130 -> 0x138 (shift +0x08)

Without this patch, init_module is never called and cleanup_module is NULL
(the module shows as [permanent] and cannot be unloaded).
Verified against deployed vkbd.ko (init=0xd4, cleanup=0x138, section=0x148).

Usage: python3 patch_module.py <module.ko>
"""
import struct, sys

OFFSET_DELTA = 0x18
PATCHES = {
    'init_module':    (0xbc, 0xd4),
    'cleanup_module': (0x130, 0x138),
}

def patch(path):
    with open(path, 'rb') as f:
        data = bytearray(f.read())

    e_shoff     = struct.unpack_from('<I', data, 0x20)[0]
    e_shentsize = struct.unpack_from('<H', data, 0x2e)[0]
    e_shnum     = struct.unpack_from('<H', data, 0x30)[0]
    e_shstrndx  = struct.unpack_from('<H', data, 0x32)[0]

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh = list(struct.unpack_from('<IIIIIIIIII', data, off))
        sections.append((sh, off))

    shstr_off = sections[e_shstrndx][0][4]

    def get_name(idx):
        end = data.index(0, shstr_off + idx)
        return data[shstr_off + idx:end].decode('utf-8', errors='replace')

    rel_sec = mod_sec = None
    for sh, file_off in sections:
        name = get_name(sh[0])
        if name == '.rel.gnu.linkonce.this_module':
            rel_sec = (sh, file_off)
        if name == '.gnu.linkonce.this_module':
            mod_sec = (sh, file_off)

    if not rel_sec or not mod_sec:
        print("ERROR: required sections not found")
        sys.exit(1)

    sh = rel_sec[0]
    sh_offset = sh[4]
    sh_size   = sh[5]
    sh_link   = sh[6]

    symtab   = sections[sh_link][0]
    sym_off  = symtab[4]
    sym_ents = symtab[9]
    strtab   = sections[symtab[6]][0]
    str_off  = strtab[4]

    mod_data_off = mod_sec[0][4]
    mod_data_size = mod_sec[0][5]
    patched_any = False

    def sym_name(idx):
        ni = struct.unpack_from('<I', data, sym_off + idx * sym_ents)[0]
        end = data.index(0, str_off + ni)
        return data[str_off + ni:end].decode('utf-8', errors='replace')

    for j in range(sh_size // 8):
        entry_off = sh_offset + j * 8
        r_off, r_info = struct.unpack_from('<II', data, entry_off)
        r_sym = r_info >> 8
        sname = sym_name(r_sym)

        if sname in PATCHES:
            expected_old, correct_new = PATCHES[sname]
            if r_off == correct_new:
                print(f"  {sname}: already at 0x{correct_new:x}")
                continue
            if r_off != expected_old:
                print(f"  WARNING: {sname} at 0x{r_off:x} (expected 0x{expected_old:x})")
            # Zero old offset only if within section bounds
            if r_off + 4 <= mod_data_size:
                struct.pack_into('<I', data, mod_data_off + r_off, 0)
            # Zero new offset only if within section bounds
            # (the kernel allocates a larger struct module, so out-of-bounds
            #  offsets are valid for the relocation but not in the .ko file)
            if correct_new + 4 <= mod_data_size:
                struct.pack_into('<I', data, mod_data_off + correct_new, 0)
            # Patch the relocation entry (always safe)
            struct.pack_into('<I', data, entry_off, correct_new)
            print(f"  {sname}: 0x{r_off:x} -> 0x{correct_new:x}")
            patched_any = True

    if patched_any:
        with open(path, 'wb') as f:
            f.write(data)
        print(f"Written: {path}")
    else:
        print(f"No patches needed: {path}")

if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <module.ko>")
        sys.exit(1)
    patch(sys.argv[1])
