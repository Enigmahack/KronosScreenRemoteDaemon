#!/usr/bin/env python3
"""
Kronos Cleaner Package Builder

Creates a USB-installable package that removes ALL non-Korg artifacts,
restoring the Kronos as close to factory state as possible without a
full firmware reinstall.

Removes:
  Root hack    — grub init=/bin/init, dropbear SSH, OA.clonos.rc, OA.clonos.si,
                 clontab, /bin/init, /bin/login, /etc/profile, /etc/shells, keymaps
                 ('clonos' files are root-hack artifacts; factory uses OA.rc / OA.si)
  ScreenRemote — /korg/rw/screenremote/, logs, kronosmods_init, kronos_init,
                 grub init=/korg/kronos_init, KronosMods Boot grub entry,
                 kronosmods injection from OA.clonos.rc (before it is deleted)
  Extraction   — KronosExtract.bin, chip_sniff_ring.bin, kronos_extract.ko

Does NOT remove:
  /sbin/busybox — factory Kronos may ship its own version; harmless to leave.

GRUB strategy:
  Patches the existing grub.conf in-place (removes init= parameters, strips
  KronosMods Boot entry, restores default=0).  Never replaces the whole file
  with a hardcoded version — Kronos hardware variants have different kernel
  parameters (memmap, vga, etc.) and a wrong replacement risks boot failure.

Safety:
  Boot-critical files (/bin/init, /etc/clontab, etc.) are only removed
  AFTER verifying grub.conf no longer references init=/bin/init or
  init=/korg (our custom init paths).

Usage:
    python build_cleaner.py [version]
"""

import shutil
import sys
import tarfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from build_package import (
    _make_pretar, _sha1_signature, _md5_file,
    _MD5SUM_PAYLOAD_REL, _DISPLAY_MSG_PAYLOAD_REL,
    GRUB_ENTRY_TITLE, GRUB_DROP_ROOTHACK_ENTRY,
)

PKG_NAME = "Kronos-Cleaner"
_DUM = "/mnt/updaterSource/DisplayUpdaterMessage"


def _make_cleaner_posttar(debug: bool = False) -> str:
    diag_dir = "/korg/rw/HD/KronosCleaner"

    lines = [
        "#!/bin/sh",
        f"# posttar.sh -- {PKG_NAME}",
        "",
        f"mkdir -p {diag_dir} 2>/dev/null || true",
        f"echo \"cleaner: started $(date)\" >> {diag_dir}/cleaner_diag.txt 2>/dev/null",
        "",
        "# -- Precondition: factory boot target must exist --",
        "if [ ! -x /sbin/init ] || [ ! -f /etc/inittab ]; then",
        f"    echo 'ABORT: /sbin/init or /etc/inittab missing' >> {diag_dir}/cleaner_diag.txt",
        "    echo 'Error: factory init missing. Clean aborted.' > /tmp/installer_status",
        f"    {_DUM} 'Error: factory init missing. Clean aborted.' 2>/dev/null",
        f"    {_DUM} 'SetDefaultPalette' 2>/dev/null",
        "    echo 'set 100' > /proc/OmapNKS4ProgressBar",
        "    exit 1",
        "fi",
        f"echo 'cleaner: precondition OK (/sbin/init + /etc/inittab)' >> {diag_dir}/cleaner_diag.txt",
        "",
        "# -- Stop non-factory processes and unload modules --",
        "# screenremote unloads midi_inject + vkbd ITSELF via delete_module(2) on",
        "# SIGTERM (restores OA's patched .text, frees trampolines).  Do NOT rmmod:",
        "# busybox rmmod reads /proc/modules, and reading /proc/modules OOPSES this",
        "# kernel whenever OA is loaded (module_refcount faults on OA's per-cpu",
        "# refptr -- OA is brought up by loadmod's custom decrypting loader).  Any",
        "# module still resident (e.g. kronos_extract) is cleared by the reboot that",
        "# completes the clean.",
        "kill $(pidof screenremote) 2>/dev/null || true",
        "kill $(pidof dropbear) 2>/dev/null || true",
        "sleep 2",
        "",
        f"{_DUM} 'Restoring factory boot...' 2>/dev/null",
        "",
        "# ====================================================================",
        "# GRUB: patch existing grub.conf to remove all custom init= parameters.",
        "# We never replace the whole file -- the user's Kronos variant may have",
        "# different kernel parameters (memmap, vga, etc.) and a wholesale replace",
        "# with a hardcoded config risks booting with wrong hardware parameters.",
        "# ====================================================================",
        "_boot_mounted=0",
        "if mount -t vfat /dev/sda1 /boot 2>/dev/null; then",
        "    _boot_mounted=1",
        f"    echo 'cleaner: mounted sda1 as vfat' >> {diag_dir}/cleaner_diag.txt",
        "elif mount /dev/sda1 /boot 2>/dev/null; then",
        "    _boot_mounted=1",
        f"    echo 'cleaner: mounted sda1 (auto-type)' >> {diag_dir}/cleaner_diag.txt",
        "else",
        f"    echo 'cleaner: WARNING: failed to mount sda1' >> {diag_dir}/cleaner_diag.txt",
        "fi",
        "",
        "GRUB_OK=0",
        "if [ -f /boot/grub/grub.conf ]; then",
        f"    cp /boot/grub/grub.conf {diag_dir}/grub_before_clean.txt 2>/dev/null || true",
        f"    echo \"cleaner: grub before patch:\" >> {diag_dir}/cleaner_diag.txt",
        f"    cat /boot/grub/grub.conf >> {diag_dir}/cleaner_diag.txt 2>/dev/null || true",
        "",
        "    # Remove the whole root-hack menu entry (kernel line with init=/bin/init)",
        "    # FIRST, while the marker is still present, so a rooted 2-entry grub.conf",
        "    # collapses back to the single factory entry.",
        f"    {GRUB_DROP_ROOTHACK_ENTRY}",
        "",
        "    # Strip any remaining init= parameters our scripts may have added.",
        "    sed -i 's| init=/bin/init||g' /boot/grub/grub.conf 2>/dev/null || true",
        "    sed -i 's| init=/korg/kronos_init||g' /boot/grub/grub.conf 2>/dev/null || true",
        "    sed -i 's| init=/korg/rw/kronos_init||g' /boot/grub/grub.conf 2>/dev/null || true",
        "",
        "    # Remove KronosMods Boot entry if present and restore original default.",
        "    # Temp file lives in /boot/grub/ (same dir/filesystem as grub.conf), not",
        "    # /tmp -- /boot is a separate vfat partition, so a /tmp temp file would",
        "    # make the mv below a cross-filesystem copy+unlink instead of an atomic",
        "    # rename(2), risking a truncated (unbootable) grub.conf on power loss.",
        f"    if grep -q '{GRUB_ENTRY_TITLE}' /boot/grub/grub.conf 2>/dev/null; then",
        "        if [ -f /korg/rw/.grub_orig_default ]; then",
        "            _ORIG=$(cat /korg/rw/.grub_orig_default 2>/dev/null)",
        f"            awk -v o=\"$_ORIG\" 'BEGIN{{skip=0}}/^default[[:space:]=]/{{print \"default \" o; next}}/^title {GRUB_ENTRY_TITLE}$/{{skip=1;next}}skip&&/^title /{{skip=0;print;next}}!skip{{print}}' \\",
        "                /boot/grub/grub.conf > /boot/grub/.grub_tmp \\",
        "                && mv /boot/grub/.grub_tmp /boot/grub/grub.conf",
        f"            echo 'cleaner: KronosMods Boot entry removed, default restored' >> {diag_dir}/cleaner_diag.txt",
        "        else",
        f"            awk 'BEGIN{{skip=0}}/^title {GRUB_ENTRY_TITLE}$/{{skip=1;next}}skip&&/^title /{{skip=0;print;next}}!skip{{print}}' \\",
        "                /boot/grub/grub.conf > /boot/grub/.grub_tmp \\",
        "                && mv /boot/grub/.grub_tmp /boot/grub/grub.conf",
        f"            echo 'cleaner: KronosMods Boot entry removed' >> {diag_dir}/cleaner_diag.txt",
        "        fi",
        "    fi",
        "",
        "    # Ensure default=0 so the factory entry boots",
        "    sed -i 's/^default.*/default=0/' /boot/grub/grub.conf 2>/dev/null || true",
        "",
        *([
            "    # Debug build: make the GRUB menu visible for diagnosis.",
            "    sed -i 's/^timeout=.*/timeout=5/' /boot/grub/grub.conf 2>/dev/null || true",
            "    sed -i '/^hiddenmenu/d' /boot/grub/grub.conf 2>/dev/null || true",
            "",
        ] if debug else []),
        "    sync",
        f"    cp /boot/grub/grub.conf {diag_dir}/grub_after_clean.txt 2>/dev/null || true",
        f"    echo \"cleaner: grub after patch:\" >> {diag_dir}/cleaner_diag.txt",
        f"    cat /boot/grub/grub.conf >> {diag_dir}/cleaner_diag.txt 2>/dev/null || true",
        "",
        "    # Verify: both custom init= values are gone before we remove their targets",
        "    if ! grep -q 'init=/bin/init' /boot/grub/grub.conf && \\",
        "       ! grep -q 'init=/korg' /boot/grub/grub.conf; then",
        "        GRUB_OK=1",
        f"        echo 'cleaner: grub.conf verified safe (no custom init=)' >> {diag_dir}/cleaner_diag.txt",
        "    else",
        f"        echo 'cleaner: WARNING: custom init= still present after patch' >> {diag_dir}/cleaner_diag.txt",
        "    fi",
        "else",
        f"    echo 'cleaner: WARNING: /boot/grub/grub.conf not found' >> {diag_dir}/cleaner_diag.txt",
        "fi",
        "",
        "[ \"$_boot_mounted\" = '1' ] && umount /boot 2>/dev/null || true",
        "",
        "# ====================================================================",
        "# Remove non-boot-critical files (safe regardless of GRUB state)",
        "# ====================================================================",
        f"{_DUM} 'Removing SSH and tools...' 2>/dev/null",
        "",
        "# Root-hack SSH / tools",
        "rm -f /bin/dropbear",
        "rm -f /bin/dbclient",
        "rm -f /bin/dropbearconvert",
        "rm -f /bin/dropbearkey",
        "rm -f /bin/scp",
        "rm -f /bin/ssh",
        "rm -f /bin/nano",
        "rm -f /bin/login",
        "rm -rf /etc/dropbear",
        "rm -rf /usr/share/keymaps",
        "rm -f /etc/profile",
        "rm -f /etc/shells",
        "rmdir /root 2>/dev/null || true",
        f"echo 'cleaner: SSH and tools removed' >> {diag_dir}/cleaner_diag.txt",
        "",
        "# ====================================================================",
        "# Remove boot-critical files ONLY if GRUB is confirmed safe.",
        "# OA.clonos.rc / OA.clonos.si are root-hack artifacts (the 'clonos'",
        "# namespace is for rooted machines only).  Factory uses OA.rc / OA.si",
        "# which we never touch.  Safe to delete once GRUB is verified.",
        "# ====================================================================",
        "if [ \"$GRUB_OK\" = '1' ]; then",
        f"    {_DUM} 'Removing root-hack boot files...' 2>/dev/null",
        "    rm -f /bin/init",
        "    rm -f /etc/OA.clonos.rc",
        "    rm -f /etc/OA.clonos.si",
        "    rm -f /etc/clontab",
        "    rm -f /etc/inittab.busybox",
        f"    echo 'cleaner: boot-critical files removed' >> {diag_dir}/cleaner_diag.txt",
        "else",
        f"    echo 'cleaner: SKIPPED boot-critical removal (GRUB not verified safe)' >> {diag_dir}/cleaner_diag.txt",
        f"    echo 'cleaner: /bin/init, clontab, OA.clonos.* left intact' >> {diag_dir}/cleaner_diag.txt",
        "fi",
        "",
        "# ====================================================================",
        "# Remove ScreenRemote + PackageMaker artifacts",
        "# ====================================================================",
        f"{_DUM} 'Removing ScreenRemote...' 2>/dev/null",
        "",
        "rm -rf /korg/rw/screenremote 2>/dev/null",
        "rm -rf /korg/rw/HD/ScreenRemote 2>/dev/null",
        "rm -f /korg/rw/kronosmods_init 2>/dev/null",
        "rm -f /korg/rw/.grub_orig_default 2>/dev/null",
        "",
        "# /korg/kronos_init is the literal init= target grub.conf's kernel line can",
        "# point at -- gate its removal behind the SAME GRUB_OK computed above (which",
        "# already checks for 'init=/korg', not just 'init=/bin/init').  This was",
        "# previously unconditional here despite this module's own docstring promising",
        "# otherwise: deleting it while a stale init=/korg/kronos_init reference",
        "# survives (e.g. because /boot failed to mount above) strands the Kronos with",
        "# an init= target that no longer exists -- unbootable.  Confirmed on real",
        "# hardware.  kronosmods_init above is NOT gated: it's only ever invoked from",
        "# inside kronos_init via '[ -x ... ] &&', so its absence is always harmless.",
        "if [ \"$GRUB_OK\" = '1' ]; then",
        "    rm -f /korg/kronos_init 2>/dev/null",
        "else",
        f"    echo 'cleaner: SKIPPED /korg/kronos_init removal (GRUB not verified safe)' >> {diag_dir}/cleaner_diag.txt",
        "fi",
        "",
        "# Clean kronosmods injection from OA.clonos.rc (if file was kept).",
        "# Temp file in /etc/ (same dir/filesystem as the target), not /tmp -- keeps",
        "# the mv below a same-filesystem atomic rename(2).",
        "if [ -f /etc/OA.clonos.rc ] && grep -q 'kronosmods' /etc/OA.clonos.rc 2>/dev/null; then",
        "    awk '!/kronosmods/' /etc/OA.clonos.rc > /etc/.OA.clonos.rc.tmp \\",
        "        && mv /etc/.OA.clonos.rc.tmp /etc/OA.clonos.rc",
        "    chmod 755 /etc/OA.clonos.rc",
        "fi",
        f"echo 'cleaner: ScreenRemote + PackageMaker artifacts removed' >> {diag_dir}/cleaner_diag.txt",
        "",
        "# ====================================================================",
        "# Remove extraction tool outputs",
        "# ====================================================================",
        "rm -f /korg/rw/KronosExtract.bin 2>/dev/null",
        "rm -f /korg/rw/chip_sniff_ring.bin 2>/dev/null",
        "rm -f /korg/rw/kronos_extract.ko 2>/dev/null",
        f"echo 'cleaner: extraction outputs removed' >> {diag_dir}/cleaner_diag.txt",
        "",
        "# Remove previous unroot diagnostics",
        "rm -rf /korg/rw/HD/Unroot 2>/dev/null",
        "",
        f"echo \"cleaner: done $(date)\" >> {diag_dir}/cleaner_diag.txt",
        f"chown -R 500:500 {diag_dir} 2>/dev/null || true",
        "sync",
        "",
        "# -- Final status --",
        "if [ \"$GRUB_OK\" = '1' ]; then",
        "    echo 'Clean complete. Please reboot.' > /tmp/installer_status",
        f"    {_DUM} 'Clean complete. Please reboot.' 2>/dev/null",
        "else",
        "    echo 'Partial clean: SSH/ScreenRemote removed but boot files kept.' > /tmp/installer_status",
        f"    {_DUM} 'Partial clean. Check diagnostics.' 2>/dev/null",
        "fi",
        f"{_DUM} 'SetDefaultPalette' 2>/dev/null",
        "sync",
        "echo 'set 0' > /proc/OmapNKS4ProgressBar",
        "_p=10",
        "for _i in 1 2 3 4 5 6 7 8 9 10; do",
        "    echo \"set $_p\" > /proc/OmapNKS4ProgressBar",
        "    sleep 1",
        "    _p=$(( _p + 9 ))",
        "done",
        "echo 'set 100' > /proc/OmapNKS4ProgressBar",
        f"{_DUM} 'SetTextPalette' 2>/dev/null",
        "",
        "exit 0",
        "",
    ]
    return "\n".join(lines)


def build(version: str = "1.0.2", debug: bool = False) -> None:
    print("=" * 52)
    print(f"  {PKG_NAME} Package Builder{'  [DEBUG]' if debug else ''}")
    print("=" * 52)
    print()

    pkg_id = f"{PKG_NAME.replace('-', '_')}_{version.replace('.', '_')}"
    tarball_name = f"{pkg_id}.tar.gz"
    out_dir = Path(__file__).parent / "dist" / pkg_id
    out_dir.mkdir(parents=True, exist_ok=True)

    tarball_path = out_dir / tarball_name
    with tarfile.open(str(tarball_path), "w:gz"):
        pass
    tarball_md5 = _md5_file(tarball_path)

    pretar_text  = _make_pretar(tarball_name, tarball_md5, PKG_NAME)
    posttar_text = _make_cleaner_posttar(debug)
    sig          = _sha1_signature(pretar_text.encode(), posttar_text.encode())

    install_info = (
        f"VERSION={PKG_NAME} {version}\n"
        f"SOURCE={tarball_name}\n"
        f"PRETARSCRIPT=pretar.sh\n"
        f"POSTTARSCRIPT=posttar.sh\n"
        f"SIGNATURE={sig}\n"
    )

    (out_dir / "pretar.sh").write_text(pretar_text,  encoding="ascii", newline="\n")
    (out_dir / "posttar.sh").write_text(posttar_text, encoding="ascii", newline="\n")
    (out_dir / "install.info").write_text(install_info, encoding="ascii", newline="\n")
    (out_dir / "mnt").mkdir(exist_ok=True)

    payload_dir = Path(__file__).parent / "payload"

    md5sum_src = payload_dir.joinpath(*_MD5SUM_PAYLOAD_REL)
    if md5sum_src.is_file():
        shutil.copy2(md5sum_src, out_dir / "md5sum")
        print(f"  md5sum: OK")
    else:
        print(f"  WARNING: md5sum not found at payload/{'/'.join(_MD5SUM_PAYLOAD_REL)}")

    display_msg_src = payload_dir.joinpath(*_DISPLAY_MSG_PAYLOAD_REL)
    if display_msg_src.is_file():
        shutil.copy2(display_msg_src, out_dir / "DisplayUpdaterMessage")
        print(f"  DisplayUpdaterMessage: OK")
    else:
        print(f"  WARNING: DisplayUpdaterMessage not found")

    print()
    print("=" * 52)
    print(f"  Ready: {out_dir}/")
    print("=" * 52)
    print()
    print("Copy to FAT32 USB root:")
    for p in sorted(out_dir.iterdir()):
        note = "  (empty -- required)" if (p.name == "mnt" and p.is_dir()) else ""
        print(f"  {p.name}{note}")
    print()
    print(f"Signature: {sig}")
    print()
    print("Removes:")
    print("  Root hack  — grub init=/bin/init, dropbear, SSH tools, OA.clonos.rc,")
    print("               OA.clonos.si, clontab, /bin/init, /bin/login,")
    print("               /etc/profile, /etc/shells, keymaps")
    print("               ('clonos' files are root-hack artifacts; factory uses OA.rc/OA.si)")
    print("  ScreenRemote — /korg/rw/screenremote/, logs, kronosmods_init,")
    print("               kronos_init, grub hooks, kronosmods injection from OA.clonos.rc")
    print("  Extraction — KronosExtract.bin, chip_sniff_ring.bin, kronos_extract.ko")
    print()
    print("Left intact:")
    print("  /sbin/busybox — may be factory; harmless to leave.")
    print()
    print("GRUB: patches existing grub.conf in-place — never replaces with hardcoded")
    print("  version.  Removes init=/bin/init, init=/korg/kronos_init, KronosMods")
    print("  Boot entry; restores default=0.  Boot-critical files only removed after")
    print("  GRUB verified safe (no custom init= remains).")
    print()
    print("Diagnostics: /korg/rw/HD/KronosCleaner/cleaner_diag.txt (FTP-accessible)")
    print()
    print("After install, reboot the Kronos to restore factory boot.")


def main() -> None:
    args = [a for a in sys.argv[1:] if a != "--debug"]
    debug = "--debug" in sys.argv[1:]
    version = args[0] if args else "1.0.0"
    build(version, debug)


if __name__ == "__main__":
    main()
