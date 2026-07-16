#!/usr/bin/env python3
"""
Kronos Package Builder
Produces USB-installable update packages for the Korg Kronos (firmware 3.2.1 /
Linux 2.6.32.11-korg, i386).

How it works
------------
UpdateOS (the Kronos update binary) reads install.info, verifies the SHA1
signature over pretar.sh + posttar.sh, extracts the tarball onto the Kronos
filesystem, then runs posttar.sh.  We use the same mechanism Korg uses for OS
updates -- no DRM bypass, no encrypted content.  See KronosRootHack/README.md
for the signature algorithm.

Layout of the output directory (copy contents to FAT32 USB root):
    install.info
    pretar.sh
    posttar.sh
    mnt/                 (empty directory -- UpdateOS requires this to exist)
    <NAME>_<VER>.tar.gz

Tarball internal paths use the "mnt/" prefix which maps to the Kronos FS root:
    mnt/korg/rw/screenremote/screenremote   -> /korg/rw/screenremote/screenremote

/korg/rw/ is NOT in the official Korg update checksum list, so files installed
there survive future Korg OS updates.

Boot hook (single in-place GRUB patch -- no scenario detection)
---------------------------------------------------------------
posttar.sh applies ONE rule to grub.conf: append  init=/korg/kronos_init  to
every kernel line that does not already carry an init= parameter.  It never adds
menu entries and never changes the 'default' line.  Consequences:

  * The set of bootable entries and the entry that boots by default are exactly
    what they were before the install.  There is no code path that can repoint
    'default' at an entry whose boot depends on our script -- the historical
    cause of bricks after cleaner/unroot -> install.
  * It is idempotent: a kernel line that already has an init= (a re-run of this
    installer, or a root-hack init=/bin/init entry) is left untouched.

On boot, kronos_init (sda2) mounts /korg/rw, launches kronosmods_init in the
BACKGROUND, then immediately exec-chains to the real init (/bin/init on a rooted
unit, else /sbin/init, else /bin/sh).  Nothing in the hook can stall the PID-1
handoff or repoint the default boot entry.

Rooted Kronos: the active entry boots init=/bin/init, so the GRUB hook never runs
at boot there.  posttar.sh therefore also injects a kronosmods_init call into
OA.clonos.rc after the STATUS=$? line (after loadoa).  Harmless on a non-rooted
unit (the line only runs if the root-hack boot chain calls the script).

Debug vs release: a debug build additionally makes the GRUB menu visible
(timeout=5, hiddenmenu removed) and writes diagnostics to
/korg/rw/HD/ScreenRemote/ (install_diag.txt, grub_pre/postinstall.txt,
kronos_boot.log, kronosmods_boot.log).  A release build changes neither the
timeout nor hiddenmenu and writes no diagnostics.

All /korg/rw/ files (kronosmods_init, kronos_init, modules, binaries) survive
Korg OS updates.  grub.conf does not -- it is reset on every Korg firmware
update, so the package must be re-run after an official update.

Legal note on grub.conf edits
------------------------------
grub.conf is part of GNU GRUB (GPLv2).  We add an  init=  kernel parameter,
a standard Linux feature for specifying an alternative init process.  We do
not modify any Korg-proprietary files, do not circumvent any encryption or
integrity check, and do not redistribute any Korg code.

Licensing note
--------------
This tool creates packages containing your own code only.  Do not include Korg
proprietary kernel modules (OA_real.ko, loadmod.ko, etc.) -- the tool will warn
if it finds known module names.  The UpdaterScriptsKey used to sign packages is
used for interoperability with UpdateOS under s.30.61 CDPA / s.1201(f) DMCA.
"""

import hashlib
import io
import os
import shutil
import sys
import tarfile
from pathlib import Path

UPDATER_SCRIPTS_KEY = bytes.fromhex("13d0afefe03c9b92162faeff775355e1")

# md5sum binary location inside the payload directory (not installed to the Kronos;
# copied to the USB root and called by pretar.sh during UpdateOS installation).
# Must be an i386 Linux ELF compatible with the Kronos (Linux 2.6.32.11, x86).
# Place it at:  <payload_dir>/md5sum/md5sum
_MD5SUM_PAYLOAD_REL = ("md5sum", "md5sum")

# DisplayUpdaterMessage binary -- controls Kronos display text and palette mode
# during the UpdateOS install process.  Without it, no status messages appear and
# OmapNKS4ProgressBar writes are invisible (requires SetDefaultPalette first).
# Place it at:  <payload_dir>/DisplayUpdaterMessage/DisplayUpdaterMessage
_DISPLAY_MSG_PAYLOAD_REL = ("DisplayUpdaterMessage", "DisplayUpdaterMessage")

# Shorthand used in generated shell scripts.
_DUM = "/mnt/updaterSource/DisplayUpdaterMessage"

# UpdateOS accepts any VERSION string — it only checks the field is present and
# does not compare it against the running firmware.  Kept here for reference only;
# install.info uses "{pkg_name} {version}" so the Kronos display shows the actual
# package identity rather than the firmware version number.
# Legacy title used by old (scenario-3) installer builds for the appended entry.
# The current installer never adds entries; cleaner/unroot still strip this title
# so machines updated from an old build are normalised.
GRUB_ENTRY_TITLE = "KronosMods Boot"

# Shell one-liner: delete the entire GRUB menu entry whose kernel line carries
# init=/bin/init (the root-hack entry), preserving the header (default=, timeout=,
# comments before the first 'title') and every other entry verbatim.  Idempotent:
# if no such entry exists the file is unchanged.  Used by the cleaner and unroot
# packages to normalise a rooted 2-entry grub.conf back toward factory shape.
#
# Temp file lives in /boot/grub/ (same directory as grub.conf itself), NOT
# /tmp: /boot is a separately mounted vfat partition (sda1), so a temp file in
# /tmp would make the final `mv` a cross-filesystem copy+unlink -- not atomic.
# A power loss mid-copy would leave a truncated, unparseable grub.conf and
# brick the boot.  Same directory guarantees `mv` is a same-filesystem
# rename(2), atomic regardless of what /tmp itself is mounted as.
GRUB_DROP_ROOTHACK_ENTRY = (
    "awk '"
    "/^title /{if(intitle&&!drop)printf \"%s\",buf; buf=$0\"\\n\"; drop=0; intitle=1; next} "
    "{if(intitle){buf=buf $0\"\\n\"; if(/init=\\/bin\\/init/)drop=1} else print} "
    "END{if(intitle&&!drop)printf \"%s\",buf}' "
    "/boot/grub/grub.conf > /boot/grub/.grub_tmp 2>/dev/null "
    "&& mv /boot/grub/.grub_tmp /boot/grub/grub.conf"
)

# Known Korg proprietary kernel module filenames (case-insensitive).
_KORG_PROPRIETARY = {
    "oa_real.ko", "oa.ko", "loadmod.ko", "omapnks4module.ko",
    "omapvideomodule.ko", "stggmp.ko", "stgenabler.ko",
    "getpubidmod.ko", "usbmidiaccessory.ko",
}


# ---------------------------------------------------------------------------
# Content generators
# ---------------------------------------------------------------------------

def _make_kronosmods_init(boot_cmds: list, debug_log: str = "") -> str:
    """
    Shell script installed at /korg/rw/kronosmods_init.

    Called (backgrounded) from /korg/kronos_init on the GRUB boot path after
    /korg/rw is mounted, or injected into OA.clonos.rc on a rooted Kronos.
    /korg/rw is already mounted in both cases.

    All real work (insmod, daemon launch) is deferred into a single background
    block that first waits for OA to come up (/proc/.oacmd appears once OA.ko is
    loaded).  Two reasons:
      * Nothing here can ever stall the caller -- the script returns immediately,
        so on the GRUB path the PID-1 handoff in kronos_init is never delayed and
        a hanging insmod cannot freeze boot.
      * Modules that need OA symbols (midi_bridge.ko) and the screenremote daemon
        load *after* OA, not racing factory module loading during early boot.
    On a rooted Kronos OA is already up when this runs (loadoa completes before
    OA.clonos.rc executes), so the wait exits on the first iteration.
    """
    import posixpath
    log_dir = posixpath.dirname(debug_log) if debug_log else ""

    lines = [
        "#!/bin/sh",
        "# kronosmods_init -- KronosMods deferred boot commands (modules + daemons).",
        "# Returns immediately; all work runs in a background block (see below).",
        "",
        "# Kill-switch: if this folder exists, load nothing.  FTP only reaches",
        "# /korg/rw/HD, so make the folder there over FTP (mkdir _nomod) and reboot",
        "# to bring the unit up with NO ScreenRemote modules/daemon -- needed to run a",
        "# Korg OS update or the cleaner safely, and to recover a unit whose modules",
        "# are wedging OA teardown.  Remove the folder and reboot to re-enable.",
        "if [ -d /korg/rw/HD/_nomod ]; then",
        "    exit 0",
        "fi",
        "",
    ]
    if debug_log:
        lines += [f"mkdir -p {log_dir} 2>/dev/null || true", ""]

    if not boot_cmds:
        if debug_log:
            lines += [f"echo 'kronosmods_init: no boot commands' >> {debug_log} 2>&1", ""]
        return "\n".join(lines)

    lines.append("(")
    if debug_log:
        lines += [
            f"    exec >> {debug_log} 2>&1",
            "    echo \"kronosmods_init: waiting for OA ($(date))\"",
        ]
    lines += [
        "    # Wait (up to 90s) for OA to load before touching modules/daemon.",
        "    _w=0",
        "    while [ ! -e /proc/.oacmd ] && [ \"$_w\" -lt 90 ]; do",
        "        sleep 1; _w=$(( _w + 1 ))",
        "    done",
    ]
    for cmd in boot_cmds:
        lines.append(f"    {cmd}")
        if debug_log:
            label = cmd.strip().split()[0] if cmd.strip() else "cmd"
            lines.append(f"    echo \"kronosmods_init: {label} exited $?\"")
    if debug_log:
        lines += [
            "    echo 'kronosmods_init: done'",
            f"    chown -R 500:500 {log_dir} 2>/dev/null || true",
        ]
    lines += [") &", ""]
    return "\n".join(lines)


def _make_kronos_init(debug: bool = False) -> str:
    """
    Shell script installed at /korg/kronos_init (root filesystem, sda2).

    Used as the GRUB init= target on the non-rooted boot path (and as a fallback
    factory entry on a rooted Kronos).  MUST live on sda2 (the root fs) -- /korg/rw
    (sda6) is not yet mounted when the kernel execs the init= target, so pointing
    init= at /korg/rw/... would ENOENT.

    This process becomes PID 1, so the cardinal rule is: it MUST always exec a real
    init, and nothing before that exec may block or fail in a way that prevents it.
    Therefore kronosmods_init is run in the BACKGROUND (a hang or module oops there
    can never stall the handoff), and the final exec falls through /bin/init ->
    /sbin/init -> /bin/sh so the device can never end up with a dead PID 1.

    Boot diagnostics are written to the FTP-readable log only in debug builds.
    """
    lines = [
        "#!/bin/sh",
        "# kronos_init -- KronosMods GRUB init= wrapper",
        "# Lives at /korg/kronos_init (root filesystem, sda2 -- NOT on /korg/rw).",
        "# grub.conf points here via  init=/korg/kronos_init  on the kernel line.",
        "",
        "# /korg/rw (/dev/sda6) is not yet mounted at init= time -- mount it now.",
        "mount -t ext3 -o commit=1,noatime /dev/sda6 /korg/rw 2>/dev/null || true",
        "",
    ]
    if debug:
        diag_dir = "/korg/rw/HD/ScreenRemote"
        lines += [
            "# Boot diagnostic.  /boot (sda1) is NOT mounted here, so log the kernel",
            "# command line (proves which GRUB entry booted) rather than grub.conf.",
            f"mkdir -p {diag_dir} 2>/dev/null || true",
            "{ echo \"kronos_init: started $(date)\"; cat /proc/cmdline; } \\",
            f"    >> {diag_dir}/kronos_boot.log 2>/dev/null || true",
            "",
        ]
    lines += [
        "# Run boot setup in the BACKGROUND so it can never stall the PID-1 handoff.",
        "[ -x /korg/rw/kronosmods_init ] && /korg/rw/kronosmods_init &",
        "",
        "# Hand off to the real init (this process becomes PID 1).  ALWAYS prefer",
        "# /sbin/init (factory sysvinit): we only get here via init=/korg/kronos_init,",
        "# which is the ScreenRemote hook on a FACTORY-shaped grub entry, so /etc/inittab",
        "# is the factory sysvinit file.  A genuinely rooted boot uses init=/bin/init",
        "# DIRECTLY (never through this script).  Crucially, a Korg OS update restores the",
        "# factory /etc/inittab but leaves the root-hack /bin/init (busybox) on disk; if we",
        "# preferred /bin/init, busybox would parse the sysvinit inittab and die with",
        "# \"Bad inittab entry at line 1, 8, 9\", breaking boot.  /bin/init is only a",
        "# fallback if /sbin/init is somehow absent; /bin/sh is the last resort so a",
        "# missing init drops to a shell instead of panicking.",
        "[ -x /sbin/init ] && exec /sbin/init \"$@\"",
        "[ -x /bin/init ]  && exec /bin/init  \"$@\"",
        "exec /bin/sh",
        "",
    ]
    return "\n".join(lines)


def _make_pretar(tarball_name: str, tarball_md5: str, pkg_name: str) -> str:
    return "\n".join([
        "#!/bin/sh",
        f"# pretar.sh -- {pkg_name}",
        "",
        "echo 'Verifying installer files...' > /tmp/installer_status",
        f"{_DUM} 'Verifying installer files...' 2>/dev/null",
        "",
        "# Abort if tarball is corrupted",
        "checksum=`/mnt/updaterSource/md5sum \"/mnt/updaterSource/" + tarball_name
        + "\" | awk '{ print $1; }'`",
        "if [ \"" + tarball_md5 + "\" != \"$checksum\" ]; then",
        "    echo 'Checksum Error! Update failed, please restart the system.' > /tmp/installer_status",
        f"    {_DUM} 'Checksum Error! Update failed, please restart the system.' 2>/dev/null",
        "    kill -9 $(/sbin/pidof UpdateOS)",
        "fi",
        "",
        f"{_DUM} 'SetTextPalette' 2>/dev/null",
        "",
        "# UpdateOS requires this directory to exist before extraction",
        "if [ ! -e /mnt/updaterSource/mnt ]; then",
        "    mkdir /mnt/updaterSource/mnt",
        "fi",
        "",
        "# Stop services that may hold file locks",
        "kill $(/sbin/pidof vsftpd)",
        "kill $(/sbin/pidof avahi-daemon)",
        "kill $(/sbin/pidof messagebus)",
        "kill $(/sbin/pidof ifplugd)",
        "sleep 2",
        "kill -9 $(/sbin/pidof vsftpd)",
        "kill -9 $(/sbin/pidof avahi-daemon)",
        "kill -9 $(/sbin/pidof messagebus)",
        "kill -9 $(/sbin/pidof ifplugd)",
        "sleep 2",
        "",
    ])


def _make_posttar(pkg_name: str, install_cmds: list, boot_hook: bool,
                  debug: bool = False) -> str:
    lines = [
        "#!/bin/sh",
        f"# posttar.sh -- {pkg_name}",
        "",
    ]

    # Fixed FTP-accessible directory used for all install-time diagnostics.
    diag_dir = "/korg/rw/HD/ScreenRemote"

    def _diag(msg):
        """Echo a diagnostic line to install_diag.txt — omitted in release builds."""
        return [f"echo \"{msg}\" >> {diag_dir}/install_diag.txt 2>/dev/null"] if debug else []

    if boot_hook:
        if debug:
            lines += [
                f"mkdir -p {diag_dir} 2>/dev/null || true",
                f"echo \"posttar: started $(date)\" >> {diag_dir}/install_diag.txt 2>/dev/null",
                "sync 2>/dev/null",
                "",
            ]

        lines += [
            "chmod 755 /korg/rw/kronosmods_init /korg/kronos_init 2>/dev/null || true",
            "",
            "# -- Rooted Kronos: also start via OA.clonos.rc -------------------------------",
            "# On a rooted Kronos the active GRUB entry boots init=/bin/init, so the GRUB",
            "# hook below never runs at boot -- OA.clonos.rc is what starts our daemon.",
            "# Injecting is harmless on a non-rooted unit (the line only runs if the",
            "# root-hack boot chain calls the script).  Idempotent via the grep guard.",
            "if [ -f /etc/OA.clonos.rc ]; then",
            *_diag("posttar: OA.clonos.rc found - injecting kronosmods_init"),
            "    if ! grep -q 'kronosmods' /etc/OA.clonos.rc; then",
            "        # Temp file in /etc/ (same dir/filesystem as the target), not /tmp --",
            "        # keeps the mv below a same-filesystem atomic rename(2) instead of a",
            "        # cross-filesystem copy+unlink that a power loss could catch mid-write.",
            "        awk '/^STATUS=/{print; print \"/korg/rw/kronosmods_init\"; next}1' \\",
            "            /etc/OA.clonos.rc > /etc/.OA.clonos.rc.tmp \\",
            "            && mv /etc/.OA.clonos.rc.tmp /etc/OA.clonos.rc",
            "        chmod 755 /etc/OA.clonos.rc",
            "        sync",
            "    fi",
            "fi",
            "",
            "# -- GRUB hook: in-place, idempotent, never destabilising --------------------",
            "# /boot (sda1) is never mounted when UpdateOS runs -- always mount it.",
            "# Rule: append  init=/korg/kronos_init  to every kernel line that does NOT",
            "# already carry an init=.  We never add menu entries and never touch the",
            "# 'default' line, so the set of bootable entries and which one boots by",
            "# default are exactly what they were before -- there is no way to repoint",
            "# 'default' at a broken entry.  Lines that already have an init= (a root-hack",
            "# init=/bin/init entry, or a previous run of this installer) are left as-is,",
            "# which makes re-running the package a no-op.",
            "_boot_mounted=0",
            "if mount -t vfat /dev/sda1 /boot 2>/dev/null; then",
            "    _boot_mounted=1",
            *_diag("posttar: mounted sda1 as vfat"),
            "elif mount /dev/sda1 /boot 2>/dev/null; then",
            "    _boot_mounted=1",
            *_diag("posttar: mounted sda1 (auto-type)"),
            "else",
            # ':' keeps the else non-empty in release builds (where _diag is empty),
            # otherwise 'else' immediately followed by 'fi' is a shell syntax error.
            *(_diag("posttar: WARNING: failed to mount /dev/sda1 at /boot") or ["    :"]),
            "fi",
            "",
            "if [ -f /boot/grub/grub.conf ]; then",
            *(
                [
                    f"    cp /boot/grub/grub.conf {diag_dir}/grub_preinstall.txt 2>/dev/null || true",
                    "    sync 2>/dev/null",
                ] if debug else []
            ),
            "    # Drop any stale wrong-path init= left by older package builds.",
            "    sed -i 's| init=/korg/rw/kronos_init||g' /boot/grub/grub.conf 2>/dev/null || true",
            "    # Temp file in /boot/grub/ (same dir/filesystem as grub.conf), not /tmp --",
            "    # /boot is a separate vfat partition, so a /tmp temp file would make the",
            "    # mv below a cross-filesystem copy+unlink instead of an atomic rename(2),",
            "    # risking a truncated grub.conf (unbootable) if power is lost mid-write.",
            "    awk '/^[[:space:]]*kernel /{if(!/init=/)print $0 \" init=/korg/kronos_init\"; else print; next}{print}' \\",
            "        /boot/grub/grub.conf > /boot/grub/.grub_tmp \\",
            "        && mv /boot/grub/.grub_tmp /boot/grub/grub.conf",
            *_diag("posttar: grub kernel-line patch exit=$?"),
        ]
        if debug:
            lines += [
                "    # Debug build only: make the GRUB menu visible (factory hides it with",
                "    # timeout=0 + hiddenmenu) so a bad boot can be observed and a good",
                "    # entry selected manually.  Release builds leave the menu untouched.",
                "    sed -i 's/^timeout=.*/timeout=5/' /boot/grub/grub.conf 2>/dev/null || true",
                "    sed -i '/^hiddenmenu/d' /boot/grub/grub.conf 2>/dev/null || true",
                "    # Make the boot VERBOSE on the LCD framebuffer console (factory boots",
                "    # loglevel=0 = silent).  Bump to 7 and drop 'fastboot' so kernel/module",
                "    # messages are visible -- lets you see exactly where a boot stalls when",
                "    # there is no serial console.  Release builds keep the quiet boot.",
                "    sed -i 's/loglevel=[0-9]*/loglevel=7/g' /boot/grub/grub.conf 2>/dev/null || true",
                "    sed -i 's/ fastboot//g' /boot/grub/grub.conf 2>/dev/null || true",
                f"    cp /boot/grub/grub.conf {diag_dir}/grub_postinstall.txt 2>/dev/null || true",
            ]
        lines += [
            "    sync",
            "else",
            *_diag("posttar: WARNING: /boot/grub/grub.conf not found after mount attempt"),
            "    echo 'kronosmods: WARNING: no grub hook installed' > /dev/kmsg 2>/dev/null || true",
            "fi",
            "",
            "[ \"$_boot_mounted\" = '1' ] && umount /boot 2>/dev/null || true",
            "",
        ]
        if debug:
            lines += [
                f"echo \"posttar: done $(date)\" >> {diag_dir}/install_diag.txt 2>/dev/null",
                f"chown -R 500:500 {diag_dir} 2>/dev/null || true",
                "sync 2>/dev/null",
                "",
            ]

    if install_cmds:
        lines.append("# Install-time commands")
        lines.extend(install_cmds)
        lines.append("")

    lines += [
        "echo 'Verifying installation...' > /tmp/installer_status",
        f"{_DUM} 'Verifying installation...' 2>/dev/null",
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


def _make_uninstall_posttar(pkg_name: str, installed_files: list,
                            boot_hook: bool, uninstall_cmds: list) -> str:
    lines = [
        "#!/bin/sh",
        f"# posttar.sh -- {pkg_name} uninstaller",
        "",
    ]

    if uninstall_cmds:
        lines += ["# Stop services / unload modules before removing files"]
        lines += uninstall_cmds
        lines += [""]

    if boot_hook:
        lines += [
            "# -- Remove boot hook (matches the in-place installer) ------------------------",
            "",
            "if [ -f /etc/OA.clonos.rc ]; then",
            "    # Remove the kronosmods_init injection from OA.clonos.rc (rooted Kronos).",
            "    # Temp file in /etc/ (same dir/filesystem as the target), not /tmp -- see",
            "    # the matching comment in the installer's OA.clonos.rc injection above.",
            "    awk '!/kronosmods/' /etc/OA.clonos.rc > /etc/.OA.clonos.rc.tmp \\",
            "        && mv /etc/.OA.clonos.rc.tmp /etc/OA.clonos.rc",
            "    chmod 755 /etc/OA.clonos.rc",
            "    sync",
            "fi",
            "",
            "# Mount sda1 (never pre-mounted) and strip our init= from every kernel line.",
            "# The installer only ever added init=/korg/kronos_init in place and never",
            "# touched 'default' or added entries, so removing the parameter fully undoes",
            "# the hook.  A root-hack init=/bin/init entry is left untouched.",
            "_boot_mounted=0",
            "if mount -t vfat /dev/sda1 /boot 2>/dev/null; then",
            "    _boot_mounted=1",
            "elif mount /dev/sda1 /boot 2>/dev/null; then",
            "    _boot_mounted=1",
            "fi",
            "# GRUB_OK gates removal of the init= target file below (see the",
            "# installed_files block): that file is what grub.conf's kernel line",
            "# execs as PID 1, so deleting it while a stale init= reference survives",
            "# (e.g. because /boot failed to mount here) would strand the Kronos with",
            "# an init= target that no longer exists -- unbootable.  Only delete it",
            "# once grub.conf is confirmed clean; same conservative rule build_cleaner.py",
            "# already uses for its own boot-critical file removal.",
            "GRUB_OK=0",
            "if [ -f /boot/grub/grub.conf ]; then",
            "    sed -i 's| init=/korg/kronos_init||g' /boot/grub/grub.conf 2>/dev/null || true",
            "    sed -i 's| init=/korg/rw/kronos_init||g' /boot/grub/grub.conf 2>/dev/null || true",
            "    sync",
            "    if ! grep -q 'init=/korg/kronos_init' /boot/grub/grub.conf 2>/dev/null && \\",
            "       ! grep -q 'init=/korg/rw/kronos_init' /boot/grub/grub.conf 2>/dev/null; then",
            "        GRUB_OK=1",
            "    fi",
            "fi",
            "[ \"$_boot_mounted\" = '1' ] && umount /boot 2>/dev/null || true",
            "",
            "# Remove stale marker from older (scenario-3) builds, and the FTP log dir.",
            "rm -f /korg/rw/.grub_orig_default 2>/dev/null || true",
            "rm -rf /korg/rw/HD/ScreenRemote 2>/dev/null || true",
            "",
        ]

    if installed_files:
        # These are the literal init= targets the installer's GRUB hook can point
        # at.  Deleting them is only safe once GRUB_OK confirms grub.conf no longer
        # references them (see the boot_hook block above) -- everything else
        # (daemon binary, kronosmods_init, etc.) is safe to remove unconditionally.
        BOOT_CRITICAL = {"/korg/kronos_init", "/korg/rw/kronos_init"}

        dirs: set = set()
        for f in installed_files:
            parts = f.split("/")
            for depth in range(4, len(parts)):
                dirs.add("/".join(parts[:depth]))

        # Direct children of /korg/rw/ (depth 4) are package-owned directories.
        # Use rm -rf so runtime-created files (e.g. extracted .ko modules) are also
        # removed.  Files that live directly under /korg/rw/ are removed individually.
        rw_subdirs = {d for d in dirs if d.count("/") == 3}   # /korg/rw/<name>
        deep_dirs  = dirs - rw_subdirs

        # Files not covered by an rw_subdir rm -rf must be removed individually.
        lone_files = [f for f in sorted(installed_files)
                      if not any(f.startswith(d + "/") for d in rw_subdirs)]
        safe_lone_files  = [f for f in lone_files if f not in BOOT_CRITICAL]
        gated_lone_files = [f for f in lone_files if f in BOOT_CRITICAL]

        if safe_lone_files:
            lines += ["# Remove installed files"]
            for f in safe_lone_files:
                lines.append(f"rm -f {f}")
            lines.append("")

        if gated_lone_files:
            if boot_hook:
                lines += ["# Boot-critical: only remove once GRUB_OK confirms grub.conf no",
                          "# longer references these as an init= target (see above).",
                          "if [ \"$GRUB_OK\" = '1' ]; then"]
                for f in gated_lone_files:
                    lines.append(f"    rm -f {f}")
                lines += [
                    "else",
                    "    echo 'Partial uninstall: grub.conf init= reference not confirmed"
                    " removed -- left init target(s) in place to avoid an unbootable init='"
                    " > /korg/rw/HD/ScreenRemote_UNINSTALL_INCOMPLETE.txt 2>/dev/null || true",
                    "fi",
                    "",
                ]
            else:
                lines += ["# Remove installed files"]
                for f in gated_lone_files:
                    lines.append(f"rm -f {f}")
                lines.append("")

        if rw_subdirs:
            lines += ["# Remove package directories (includes any runtime-created files)"]
            for d in sorted(rw_subdirs):
                lines.append(f"rm -rf {d} 2>/dev/null")
            lines.append("")

        if deep_dirs:
            lines += ["# Remove any deeper directories we created if now empty"]
            for d in sorted(deep_dirs, key=lambda x: x.count("/"), reverse=True):
                lines.append(f"rmdir {d} 2>/dev/null")
            lines.append("")

    lines += [
        "echo 'Verifying uninstall...' > /tmp/installer_status",
        f"{_DUM} 'Verifying uninstall...' 2>/dev/null",
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


def _build_uninstaller(pkg_name: str, version: str, installed_files: list,
                       boot_hook: bool, uninstall_cmds: list,
                       md5sum_src: Path = None,
                       display_msg_src: Path = None) -> Path:
    upkg_id       = f"{pkg_name.replace(' ', '_').replace('/', '_')}_{version.replace('.', '_')}_Uninstall"
    utarball_name = f"{upkg_id}.tar.gz"
    uout_dir      = Path(__file__).parent / "dist" / upkg_id
    uout_dir.mkdir(parents=True, exist_ok=True)

    utarball_path = uout_dir / utarball_name
    with tarfile.open(str(utarball_path), "w:gz"):
        pass
    utarball_md5 = _md5_file(utarball_path)

    upretar  = _make_pretar(utarball_name, utarball_md5, f"{pkg_name} Uninstaller")
    uposttar = _make_uninstall_posttar(pkg_name, installed_files,
                                       boot_hook, uninstall_cmds)
    usig     = _sha1_signature(upretar.encode(), uposttar.encode())

    uinfo = (
        f"VERSION={pkg_name} {version} Uninstall\n"
        f"SOURCE={utarball_name}\n"
        f"PRETARSCRIPT=pretar.sh\n"
        f"POSTTARSCRIPT=posttar.sh\n"
        f"SIGNATURE={usig}\n"
    )

    (uout_dir / "pretar.sh").write_text(upretar,  encoding="ascii", newline="\n")
    (uout_dir / "posttar.sh").write_text(uposttar, encoding="ascii", newline="\n")
    (uout_dir / "install.info").write_text(uinfo,  encoding="ascii", newline="\n")
    (uout_dir / "mnt").mkdir(exist_ok=True)

    if md5sum_src and md5sum_src.is_file():
        shutil.copy2(md5sum_src, uout_dir / "md5sum")
    if display_msg_src and display_msg_src.is_file():
        shutil.copy2(display_msg_src, uout_dir / "DisplayUpdaterMessage")

    print(f"  Uninstaller: {uout_dir}/  (sig: {usig[:16]}...)")
    return uout_dir


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _sha1_signature(pretar: bytes, posttar: bytes) -> str:
    return hashlib.sha1(pretar + posttar + UPDATER_SCRIPTS_KEY).hexdigest()


def _md5_file(path: Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _build_tarball(payload_dir: Path, out_path: Path,
                   extra_files: dict = None) -> None:
    def _mode_for(arcname: str) -> int:
        fname = arcname.rsplit("/", 1)[-1]
        if arcname.endswith(".ko") or "/bin/" in arcname or "/sbin/" in arcname \
                or "." not in fname:
            return 0o100755
        return 0o100644

    with tarfile.open(out_path, "w:gz") as tar:
        for root, dirs, files in os.walk(payload_dir):
            dirs.sort()
            for fname in sorted(files):
                fpath = Path(root) / fname
                arcname = fpath.relative_to(payload_dir).as_posix()
                if not arcname.startswith("mnt/"):
                    continue  # only mnt/ content goes in the tarball
                ti = tar.gettarinfo(str(fpath), arcname=arcname)
                ti.mode = _mode_for(arcname)
                with open(str(fpath), "rb") as fh:
                    tar.addfile(ti, fh)

        if extra_files:
            for arcname, entry in sorted(extra_files.items()):
                if isinstance(entry, (str, bytes)):
                    content, mode = entry, 0o755
                else:
                    content, mode = entry
                if isinstance(content, str):
                    content = content.encode()
                info = tarfile.TarInfo(name=arcname)
                info.size = len(content)
                info.mode = mode
                tar.addfile(info, fileobj=io.BytesIO(content))


def _check_payload_licensing(payload_dir: Path) -> None:
    found = []
    for root, _, files in os.walk(payload_dir):
        for fname in files:
            if fname.lower() in _KORG_PROPRIETARY:
                found.append(str(Path(root) / fname))
    if not found:
        return
    print()
    print("WARNING: payload may contain Korg proprietary files:")
    for p in found:
        print(f"  {p}")
    print("  Redistributing Korg kernel modules may violate their copyright.")
    print("  Only include your own code.")
    ans = input("  Continue anyway? [y/N]: ").strip().lower()
    if ans != "y":
        sys.exit("Aborted.")


def _prompt(label: str, default: str = "") -> str:
    suffix = f" [{default}]" if default else ""
    val = input(f"{label}{suffix}: ").strip()
    return val if val else default


def _prompt_yn(label: str, default: bool = True) -> bool:
    hint = "[Y/n]" if default else "[y/N]"
    val = input(f"{label} {hint}: ").strip().lower()
    return default if not val else val.startswith("y")


def _auto_detect_commands(payload_dir: Path) -> tuple:
    """
    Scan payload for files under mnt/korg/rw/ and derive boot + uninstall commands.

    Rules (applied only to mnt/korg/rw/ paths):
      *.ko anywhere              -> insmod at boot (modules first); NOT rmmod'd at
                                    uninstall (see below) -- cleared at reboot
      no extension, anywhere     -> ... & at boot; kill $(pidof ...) at uninstall

    CORRECTED 2026-07-16 (was a misunderstanding, confirmed by live testing):
    `rmmod`/`lsmod` do NOT oops this kernel "whenever OA is loaded" -- that
    earlier claim was an overgeneralization of a real but much narrower hazard.
    The actual mechanism (see screenremote.c's wait_for_oa_live() comment,
    confirmed against a real oops in boot_kmsg.log: "module_refcount+0x25"):
    reading /proc/modules makes m_show() call module_refcount() on every
    module, which faults if that module is still COMING (mid-init, per-cpu
    refptr not yet mapped) -- NOT once it has reached LIVE.  By the time an
    uninstaller runs, OA is normally already LIVE, so rmmod is safe here; live
    rmmod testing on real hardware on 2026-07-16 confirmed no oops.  This
    function still doesn't emit rmmod (reboot clears the module either way,
    and the daemon already unloads its own modules itself via delete_module(2)
    on SIGTERM -- see screenremote.c's unload_our_modules()/graceful_shutdown())
    -- that's a deliberate simplicity choice now, not a safety requirement.

    Returns (boot_cmds, uninstall_cmds, daemon_paths).
    """
    mods: list    = []   # (kronos_path, module_name)
    daemons: list = []   # (kronos_path, binary_name)

    for root, dirs, files in os.walk(payload_dir):
        dirs.sort()
        for fname in sorted(files):
            fpath   = Path(root) / fname
            arcname = fpath.relative_to(payload_dir).as_posix()
            if not arcname.startswith("mnt/korg/rw/"):
                continue
            kpath = "/" + arcname[4:]   # /korg/rw/...

            if fname.endswith(".ko"):
                mods.append((kpath, fname[:-3]))
            elif "." not in fname:
                daemons.append((kpath, fname))

    boot_cmds: list    = []
    uninstall_cmds: list = []
    daemon_paths: list = []

    for kpath, modname in sorted(mods):
        boot_cmds.append(f"insmod {kpath} 2>/dev/null || true")
        # No rmmod here: not a safety requirement (rmmod is fine once OA is
        # LIVE, see docstring) -- just simplicity, since the module is cleared
        # by the post-uninstall reboot regardless.
        uninstall_cmds.append(
            f"# {modname}: unloaded at reboot (not live-rmmod'd, see docstring)")

    for kpath, name in sorted(daemons):
        boot_cmds.append(f"pidof {name} > /dev/null 2>&1 || {kpath} &")
        uninstall_cmds.append(f"kill $(pidof {name}) 2>/dev/null || true")
        daemon_paths.append(kpath)

    return boot_cmds, uninstall_cmds, daemon_paths


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def _build_deployment() -> None:
    """Interactive flow for building a payload deployment package."""
    pkg_name = _prompt("Package name (e.g. ScreenRemote, VKeyboard)")
    if not pkg_name:
        sys.exit("Package name is required.")
    # Sanitize for filenames and shell scripts — spaces break shell word-splitting
    # in pretar.sh and UpdateOS's SOURCE= parsing.  Display name keeps original.
    pkg_name_id = pkg_name.replace(" ", "_").replace("/", "_")
    if pkg_name_id != pkg_name:
        print(f"  (filename ID: {pkg_name_id})")

    version = _prompt("Version", "1.0.0")

    default_payload = str(Path(__file__).parent / "payload")
    payload_str = _prompt("Payload directory (files organised under mnt/ prefix)", default_payload)
    payload_dir = Path(payload_str)
    if not payload_dir.is_dir():
        sys.exit(f"Error: '{payload_dir}' is not a directory.")

    _check_payload_licensing(payload_dir)

    # Auto-detect boot and uninstall commands from the payload contents.
    boot_cmds, uninstall_cmds, daemon_paths = _auto_detect_commands(payload_dir)

    print()
    if boot_cmds:
        print("Auto-detected boot commands:")
        for cmd in boot_cmds:
            print(f"  {cmd}")
        print("Auto-detected uninstall commands:")
        for cmd in uninstall_cmds:
            print(f"  {cmd}")
    else:
        print("(No .ko modules or /bin/ executables found in payload.)")

    print()
    boot_hook = _prompt_yn(
        "Add boot hook?  (N = file-delivery only, no startup hook)",
        default=bool(boot_cmds),
    )

    if not boot_hook:
        boot_cmds      = []
        uninstall_cmds = []

    # Explicitly chmod executables in posttar.sh so they are runnable even if
    # UpdateOS doesn't preserve tarball mode bits on the target filesystem.
    install_cmds: list = [
        f"chmod 755 {path} 2>/dev/null || true"
        for path in daemon_paths
    ]

    pkg_id       = f"{pkg_name_id}_{version.replace('.', '_')}"
    tarball_name = f"{pkg_id}.tar.gz"
    out_dir      = Path(__file__).parent / "dist" / pkg_id
    out_dir.mkdir(parents=True, exist_ok=True)

    debug = False
    debug_log = ""
    if boot_hook:
        debug = _prompt_yn(
            "Debug build? (visible GRUB timeout=5 + install/boot diagnostics to /korg/rw/HD/)",
            default=False,
        )
        if debug:
            debug_log = "/korg/rw/HD/ScreenRemote/kronosmods_boot.log"

    extra_files: dict = {}
    if boot_hook:
        extra_files["mnt/korg/rw/kronosmods_init"] = (_make_kronosmods_init(boot_cmds, debug_log), 0o755)
        extra_files["mnt/korg/kronos_init"]         = (_make_kronos_init(debug),                   0o755)

    tarball_path = out_dir / tarball_name
    print(f"\nBuilding {tarball_name} ...")
    _build_tarball(payload_dir, tarball_path, extra_files)
    tarball_md5 = _md5_file(tarball_path)
    print(f"  MD5: {tarball_md5}")

    pretar_text  = _make_pretar(tarball_name, tarball_md5, pkg_name)
    posttar_text = _make_posttar(pkg_name, install_cmds, boot_hook, debug)
    sig          = _sha1_signature(pretar_text.encode(), posttar_text.encode())

    install_info = (
        f"VERSION={pkg_name} {version}\n"
        f"SOURCE={tarball_name}\n"
        f"PRETARSCRIPT=pretar.sh\n"
        f"POSTTARSCRIPT=posttar.sh\n"
        f"SIGNATURE={sig}\n"
    )

    (out_dir / "pretar.sh").write_text(pretar_text,  encoding="ascii", newline="\n")
    (out_dir / "posttar.sh").write_text(posttar_text, encoding="ascii", newline="\n")
    (out_dir / "install.info").write_text(install_info, encoding="ascii", newline="\n")
    (out_dir / "mnt").mkdir(exist_ok=True)

    md5sum_src = payload_dir.joinpath(*_MD5SUM_PAYLOAD_REL)
    if md5sum_src.is_file():
        shutil.copy2(md5sum_src, out_dir / "md5sum")
        print(f"  md5sum: copied from payload/{'/'.join(_MD5SUM_PAYLOAD_REL)}")
    else:
        print(f"  WARNING: md5sum binary not found at payload/{'/'.join(_MD5SUM_PAYLOAD_REL)}")
        print(f"           pretar.sh will fail on install -- place the i386 Linux md5sum there.")

    display_msg_src = payload_dir.joinpath(*_DISPLAY_MSG_PAYLOAD_REL)
    if display_msg_src.is_file():
        shutil.copy2(display_msg_src, out_dir / "DisplayUpdaterMessage")
        print(f"  DisplayUpdaterMessage: copied from payload/{'/'.join(_DISPLAY_MSG_PAYLOAD_REL)}")
    else:
        print(f"  WARNING: DisplayUpdaterMessage binary not found at payload/{'/'.join(_DISPLAY_MSG_PAYLOAD_REL)}")
        print(f"           Install will work but display no status text or progress bar.")

    installed_files: list = []
    for root, _, files in os.walk(payload_dir):
        for fname in files:
            fpath = Path(root) / fname
            arcname = fpath.relative_to(payload_dir).as_posix()
            if arcname.startswith("mnt/korg/rw/"):
                installed_files.append("/" + arcname[4:])
    if boot_hook:
        installed_files.append("/korg/rw/kronosmods_init")
        installed_files.append("/korg/kronos_init")

    print("\nBuilding uninstaller ...")
    _build_uninstaller(pkg_name, version, installed_files, boot_hook, uninstall_cmds,
                       md5sum_src, display_msg_src)

    print()
    print("=" * 52)
    print(f"  Ready: {out_dir}/")
    print("=" * 52)
    print()
    print("Copy these to the root of a FAT32 USB drive:")
    for p in sorted(out_dir.iterdir()):
        note = "  (empty -- required)" if (p.name == "mnt" and p.is_dir()) else ""
        print(f"  {p.name}{note}")
    print()
    print(f"Signature: {sig}")

    if boot_hook:
        print()
        print("Boot hook (in-place GRUB patch, applied at install time on the Kronos):")
        print("  GRUB  -- appends  init=/korg/kronos_init  to every kernel line that has")
        print("           no init= yet.  Never adds entries, never changes 'default'.")
        print("           Idempotent: re-running is a no-op.  A root-hack init=/bin/init")
        print("           entry is left untouched.")
        print("  Rooted -- also injects kronosmods_init into OA.clonos.rc after loadoa,")
        print("           since a rooted unit boots init=/bin/init (the GRUB hook never")
        print("           runs at boot there).")
        print()
        print("  /korg/kronos_init lives on sda2 (root fs) -- accessible at init= time.")
        print("  /korg/rw/ files (kronosmods_init, daemon) survive Korg OS updates.")
        print("  grub.conf is reset by Korg OS updates -- re-run this package afterwards.")
        if debug:
            print()
            print("  DEBUG build: GRUB menu made visible (timeout=5, hiddenmenu removed).")
            print("  Diagnostics in /korg/rw/HD/ScreenRemote/ (FTP-accessible):")
            print("    install_diag.txt    -- install-time steps + exit codes")
            print("    grub_preinstall.txt -- grub.conf before posttar.sh")
            print("    grub_postinstall.txt-- grub.conf after posttar.sh")
            print("    kronos_boot.log     -- /proc/cmdline, appended on every boot")
            print("    kronosmods_boot.log -- daemon/module output, appended on every boot")
        else:
            print()
            print("  RELEASE build: GRUB menu left as-is (no timeout change), no diagnostics.")


def _build_uninstaller_only() -> None:
    """Interactive flow for building a standalone uninstaller package."""
    pkg_name = _prompt("Package name to uninstall (e.g. ScreenRemote)")
    if not pkg_name:
        sys.exit("Package name is required.")
    pkg_name_id = pkg_name.replace(" ", "_").replace("/", "_")
    if pkg_name_id != pkg_name:
        print(f"  (filename ID: {pkg_name_id})")

    version = _prompt("Version", "1.0.0")

    default_payload = str(Path(__file__).parent / "payload")
    payload_str = _prompt("Payload directory (matching the installed package)", default_payload)
    payload_dir = Path(payload_str)
    if not payload_dir.is_dir():
        sys.exit(f"Error: '{payload_dir}' is not a directory.")

    boot_cmds, uninstall_cmds, daemon_paths = _auto_detect_commands(payload_dir)

    print()
    if uninstall_cmds:
        print("Auto-detected uninstall commands:")
        for cmd in uninstall_cmds:
            print(f"  {cmd}")
    else:
        print("(No .ko modules or executables found in payload.)")

    print()
    boot_hook = _prompt_yn(
        "Remove boot hook?  (Y = uninstall will undo grub/OA.clonos.rc changes)",
        default=bool(boot_cmds),
    )

    if not boot_hook:
        uninstall_cmds = []

    installed_files: list = []
    for root, _, files in os.walk(payload_dir):
        for fname in files:
            fpath = Path(root) / fname
            arcname = fpath.relative_to(payload_dir).as_posix()
            if arcname.startswith("mnt/korg/rw/"):
                installed_files.append("/" + arcname[4:])
    if boot_hook:
        installed_files.append("/korg/rw/kronosmods_init")
        installed_files.append("/korg/kronos_init")

    md5sum_src = payload_dir.joinpath(*_MD5SUM_PAYLOAD_REL)
    display_msg_src = payload_dir.joinpath(*_DISPLAY_MSG_PAYLOAD_REL)

    print("\nBuilding uninstaller ...")
    uout_dir = _build_uninstaller(pkg_name, version, installed_files, boot_hook,
                                  uninstall_cmds, md5sum_src, display_msg_src)

    print()
    print("=" * 52)
    print(f"  Ready: {uout_dir}/")
    print("=" * 52)
    print()
    print("Copy to FAT32 USB root:")
    for p in sorted(uout_dir.iterdir()):
        note = "  (empty -- required)" if (p.name == "mnt" and p.is_dir()) else ""
        print(f"  {p.name}{note}")


def main() -> None:
    print("=" * 52)
    print("  Kronos Package Builder")
    print("=" * 52)
    print()
    print("Select package type:")
    print("  1. Factory State Restore  (remove ALL non-Korg artifacts)")
    print("  2. Package Deployment     (install payload with optional boot hook)")
    print("  3. Root Cleaner           (remove root hack, keep ScreenRemote)")
    print("  4. Uninstaller            (build standalone uninstaller for a package)")
    print()

    choice = _prompt("Package type", "1")

    if choice == "1":
        version = _prompt("Version", "1.0.0")
        debug = _prompt_yn("Debug build? (visible GRUB timeout=5 for recovery)", default=False)
        print()
        from build_cleaner import build as build_cleaner
        build_cleaner(version, debug)

    elif choice == "2":
        print()
        _build_deployment()

    elif choice == "3":
        version = _prompt("Version", "1.0.0")
        debug = _prompt_yn("Debug build? (visible GRUB timeout=5 for recovery)", default=False)
        print()
        from build_unroot import build as build_unroot
        build_unroot(version, debug)

    elif choice == "4":
        print()
        _build_uninstaller_only()

    else:
        sys.exit(f"Invalid choice: {choice}")


if __name__ == "__main__":
    main()
