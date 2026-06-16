#!/usr/bin/env python3
"""
patch_init_offset.py

The Kronos kernel's struct module has mod->init at offset 0xd4,
but GCC 13 / vanilla 2.6.32 headers place it at 0xbc.
This script patches the .rel.gnu.linkonce.this_module relocation
entry for init_module from offset 0xbc -> 0xd4 so the kernel
actually calls init_module when the module is loaded.

Usage: python3 patch_init_offset.py path_to_module.ko
"""
import struct, sys, os

CORRECT_INIT_OFFSET = 0xd4

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
        sections.append((sh, e_shoff + i * e_shentsize))

    shstr_off = sections[e_shstrndx][0][4]

    def get_name(idx):
        end = data.index(0, shstr_off + idx)
        return data[shstr_off + idx:end].decode('utf-8', errors='replace')

    rel_sec = None
    mod_sec = None
    for sh, file_off in sections:
        name = get_name(sh[0])
        if name == '.rel.gnu.linkonce.this_module':
            rel_sec = (sh, file_off)
        if name == '.gnu.linkonce.this_module':
            mod_sec = (sh, file_off)

    if not rel_sec:
        print("ERROR: .rel.gnu.linkonce.this_module not found")
        sys.exit(1)
    if not mod_sec:
        print("ERROR: .gnu.linkonce.this_module not found")
        sys.exit(1)

    sh = rel_sec[0]
    sh_offset = sh[4]
    sh_size   = sh[5]
    sh_link   = sh[6]  # symbol table index

    symtab    = sections[sh_link][0]
    sym_off   = symtab[4]
    sym_ents  = symtab[9]
    strtab    = sections[symtab[6]][0]
    str_off   = strtab[4]

    def sym_name(idx):
        ni  = struct.unpack_from('<I', data, sym_off + idx * sym_ents)[0]
        end = data.index(0, str_off + ni)
        return data[str_off + ni:end].decode('utf-8', errors='replace')

    mod_data_off = mod_sec[0][4]
    patched = False

    for j in range(sh_size // 8):
        entry_off = sh_offset + j * 8
        r_off, r_info = struct.unpack_from('<II', data, entry_off)
        r_sym  = r_info >> 8
        r_type = r_info & 0xff
        sname  = sym_name(r_sym)

        if sname == 'init_module':
            print(f"Found init_module relocation at r_offset=0x{r_off:x}")
            if r_off == CORRECT_INIT_OFFSET:
                print("Already at correct offset 0xd4 - no patch needed.")
                return
            # Zero out the value at the OLD offset in the section data
            # (the compiler put 0 there anyway for a relocation, but be safe)
            struct.pack_into('<I', data, mod_data_off + r_off, 0)
            # Zero out the value at the NEW offset so relocation writes cleanly
            existing = struct.unpack_from('<I', data, mod_data_off + CORRECT_INIT_OFFSET)[0]
            if existing != 0:
                print(f"  Zeroing section data at 0x{CORRECT_INIT_OFFSET:x} "
                      f"(was 0x{existing:08x})")
                struct.pack_into('<I', data, mod_data_off + CORRECT_INIT_OFFSET, 0)
            # Patch the relocation offset
            struct.pack_into('<I', data, entry_off, CORRECT_INIT_OFFSET)
            print(f"  Patched: r_offset 0x{r_off:x} -> 0x{CORRECT_INIT_OFFSET:x}")
            patched = True
            break

    if not patched:
        print("ERROR: init_module relocation entry not found")
        sys.exit(1)

    with open(path, 'wb') as f:
        f.write(data)
    print(f"Written: {path}")

if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <module.ko>")
        sys.exit(1)
    patch(sys.argv[1])
