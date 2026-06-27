#!/usr/bin/env python3
"""
Kronos Unroot Package Builder

Creates a USB-installable package that reverses the KronosRootHack,
restoring the factory boot chain.

What it does:
  - Patches grub.conf to remove init=/bin/init and set default=0
  - Removes SSH server (dropbear) and related tools
  - Removes root-hack init scripts (/bin/init, OA.clonos.rc, OA.clonos.si,
    clontab, inittab.busybox) — 'clonos' files are root-hack artifacts only;
    factory boot uses OA.rc / OA.si which are never touched
  - Kills running dropbear process

Preserves PackageMaker / ScreenRemote hooks:
  - /korg/kronos_init, /korg/rw/kronosmods_init left intact
  - init=/korg/kronos_init kept in grub.conf kernel lines
  - KronosMods Boot grub entry kept if present

What it does NOT do:
  - Replace /sbin/busybox (may differ from factory; harmless to leave)
  - Replace /etc/profile or /etc/shells (may be factory files)
  - Full byte-for-byte factory restore (only a Korg firmware reinstall does that)

Safety:
  Boot-critical files (/bin/init, /etc/clontab, etc.) are only removed
  AFTER verifying grub.conf no longer references init=/bin/init.
  If the grub patch fails, those files are left intact so the device
  still boots the rooted chain instead of bricking.

Usage:
    python build_unroot.py [version]
"""

import shutil
import sys
import tarfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from build_package import (
    _make_pretar, _sha1_signature, _md5_file,
    _MD5SUM_PAYLOAD_REL, _DISPLAY_MSG_PAYLOAD_REL,
    GRUB_DROP_ROOTHACK_ENTRY,
)

PKG_NAME = "Kronos-Unroot"
_DUM = "/mnt/updaterSource/DisplayUpdaterMessage"


def _make_unroot_posttar(debug: bool = False) -> str:
    diag_dir = "/korg/rw/HD/Unroot"

    lines = [
        "#!/bin/sh",
        f"# posttar.sh -- {PKG_NAME}",
        "",
        f"mkdir -p {diag_dir} 2>/dev/null || true",
        f"echo \"unroot: started $(date)\" >> {diag_dir}/unroot_diag.txt 2>/dev/null",
        "",
        "# -- Precondition: factory boot target must exist --",
        "if [ ! -x /sbin/init ] || [ ! -f /etc/inittab ]; then",
        f"    echo 'ABORT: /sbin/init or /etc/inittab missing' >> {diag_dir}/unroot_diag.txt",
        "    echo 'Error: factory init missing. Unroot aborted.' > /tmp/installer_status",
        f"    {_DUM} 'Error: factory init missing. Unroot aborted.' 2>/dev/null",
        f"    {_DUM} 'SetDefaultPalette' 2>/dev/null",
        "    echo 'set 100' > /proc/OmapNKS4ProgressBar",
        "    exit 1",
        "fi",
        f"echo 'unroot: precondition OK (/sbin/init + /etc/inittab)' >> {diag_dir}/unroot_diag.txt",
        "",
        "# -- Kill dropbear SSH --",
        "kill $(pidof dropbear) 2>/dev/null || true",
        "",
        f"{_DUM} 'Restoring factory boot...' 2>/dev/null",
        "",
        "# -- Mount sda1 and patch grub.conf --",
        "_boot_mounted=0",
        "if mount -t vfat /dev/sda1 /boot 2>/dev/null; then",
        "    _boot_mounted=1",
        f"    echo 'unroot: mounted sda1 as vfat' >> {diag_dir}/unroot_diag.txt",
        "elif mount /dev/sda1 /boot 2>/dev/null; then",
        "    _boot_mounted=1",
        f"    echo 'unroot: mounted sda1 (auto-type)' >> {diag_dir}/unroot_diag.txt",
        "else",
        f"    echo 'unroot: WARNING: failed to mount sda1' >> {diag_dir}/unroot_diag.txt",
        "fi",
        "",
        "GRUB_OK=0",
        "if [ -f /boot/grub/grub.conf ]; then",
        f"    cp /boot/grub/grub.conf {diag_dir}/grub_before_unroot.txt 2>/dev/null || true",
        "",
        "    # Remove the whole root-hack menu entry (the one with init=/bin/init), so a",
        "    # rooted 2-entry grub.conf collapses back to the single factory entry.  This",
        "    # is what makes a later ScreenRemote install see a clean factory-shaped file.",
        "    # ScreenRemote's own entry (init=/korg/kronos_init) is preserved.",
        f"    {GRUB_DROP_ROOTHACK_ENTRY}",
        "",
        "    # Belt-and-suspenders: strip any stray init=/bin/init left on a kept line.",
        "    sed -i 's| init=/bin/init||g' /boot/grub/grub.conf 2>/dev/null || true",
        "",
        "    # Set default=0 (factory entry)",
        "    sed -i 's/^default.*/default=0/' /boot/grub/grub.conf 2>/dev/null || true",
        "",
    ]
    if debug:
        lines += [
            "    # Debug build: make the GRUB menu visible for diagnosis.",
            "    sed -i 's/^timeout=.*/timeout=5/' /boot/grub/grub.conf 2>/dev/null || true",
            "    sed -i '/^hiddenmenu/d' /boot/grub/grub.conf 2>/dev/null || true",
        ]
    lines += [
        "    sync",
        f"    cp /boot/grub/grub.conf {diag_dir}/grub_after_unroot.txt 2>/dev/null || true",
        "",
        "    # Verify the patch succeeded",
        "    if ! grep -q 'init=/bin/init' /boot/grub/grub.conf; then",
        "        GRUB_OK=1",
        f"        echo 'unroot: grub.conf patched OK' >> {diag_dir}/unroot_diag.txt",
        "    else",
        f"        echo 'unroot: WARNING: init=/bin/init still present after patch' >> {diag_dir}/unroot_diag.txt",
        "    fi",
        "else",
        f"    echo 'unroot: WARNING: /boot/grub/grub.conf not found' >> {diag_dir}/unroot_diag.txt",
        "fi",
        "",
        "[ \"$_boot_mounted\" = '1' ] && umount /boot 2>/dev/null || true",
        "",
        f"{_DUM} 'Removing SSH and tools...' 2>/dev/null",
        "",
        "# -- Remove non-boot-critical files (safe regardless of GRUB state) --",
        "rm -f /bin/dropbear",
        "rm -f /bin/dbclient",
        "rm -f /bin/dropbearconvert",
        "rm -f /bin/dropbearkey",
        "rm -f /bin/scp",
        "rm -f /bin/ssh",
        "rm -f /bin/nano",
        "rm -rf /etc/dropbear",
        "rm -rf /usr/share/keymaps",
        "rmdir /root 2>/dev/null || true",
        f"echo 'unroot: SSH and tools removed' >> {diag_dir}/unroot_diag.txt",
        "",
        "# -- Remove boot-critical files ONLY if GRUB is confirmed safe --",
        "# If GRUB still points to init=/bin/init, these files are the only",
        "# thing keeping the device bootable.  Leave them to avoid a brick.",
        "# OA.clonos.rc / OA.clonos.si are root-hack artifacts (the 'clonos'",
        "# namespace is for rooted machines only).  Factory uses OA.rc / OA.si",
        "# which we never touch.  Safe to delete once GRUB is verified.",
        "if [ \"$GRUB_OK\" = '1' ]; then",
        f"    {_DUM} 'Removing root-hack boot files...' 2>/dev/null",
        "    rm -f /bin/init",
        "    rm -f /etc/OA.clonos.rc",
        "    rm -f /etc/OA.clonos.si",
        "    rm -f /etc/clontab",
        "    rm -f /etc/inittab.busybox",
        f"    echo 'unroot: boot-critical files removed' >> {diag_dir}/unroot_diag.txt",
        "else",
        f"    echo 'unroot: SKIPPED boot-critical removal (GRUB not verified safe)' >> {diag_dir}/unroot_diag.txt",
        f"    echo 'unroot: /bin/init, clontab, OA.clonos.* left intact' >> {diag_dir}/unroot_diag.txt",
        "fi",
        "",
        f"echo \"unroot: done $(date)\" >> {diag_dir}/unroot_diag.txt",
        f"chown -R 500:500 {diag_dir} 2>/dev/null || true",
        "sync",
        "",
        "# -- Final status --",
        "if [ \"$GRUB_OK\" = '1' ]; then",
        "    echo 'Unroot complete. Please reboot.' > /tmp/installer_status",
        f"    {_DUM} 'Unroot complete. Please reboot.' 2>/dev/null",
        "else",
        "    echo 'Partial unroot: SSH removed but boot files kept.' > /tmp/installer_status",
        f"    {_DUM} 'Partial unroot. Check diagnostics.' 2>/dev/null",
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
    posttar_text = _make_unroot_posttar(debug)
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
    print("Actions:")
    print("  1. Patches grub.conf: removes the root-hack entry (init=/bin/init),")
    print("     sets default=0" + ("; sets timeout=5 + shows menu (DEBUG)" if debug else " (menu/timeout left as-is)"))
    print("  2. Removes dropbear SSH + tools (ssh, scp, nano, dbclient)")
    print("  3. Removes root-hack boot chain (/bin/init, OA.clonos.rc, OA.clonos.si,")
    print("               clontab, inittab.busybox)")
    print("     ^ only after GRUB is verified safe (no brick risk)")
    print()
    print("Preserved (ScreenRemote / PackageMaker hooks):")
    print("  /korg/kronos_init, /korg/rw/kronosmods_init")
    print("  init=/korg/kronos_init in grub.conf, KronosMods Boot entry")
    print()
    print("Left intact (harmless, Korg firmware reinstall restores originals):")
    print("  /sbin/busybox, /etc/profile, /etc/shells, /bin/login")
    print()
    print("Diagnostics: /korg/rw/HD/Unroot/unroot_diag.txt (FTP-accessible)")
    print()
    print("After install, reboot the Kronos to restore factory boot.")


def main() -> None:
    args = [a for a in sys.argv[1:] if a != "--debug"]
    debug = "--debug" in sys.argv[1:]
    version = args[0] if args else "1.0.0"
    build(version, debug)


if __name__ == "__main__":
    main()
