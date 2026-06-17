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

Boot hook (three scenarios, detected automatically at install time)
-------------------------------------------------------------------
Scenario 1 -- Factory (non-rooted) Kronos, standard grub.conf:
    posttar.sh appends  init=/korg/kronos_init  to the kernel line in
    grub.conf.  On boot, kronos_init mounts /korg/rw, runs kronosmods_init
    (insmod / start services), then exec-chains to /sbin/init.

    NOTE: Korg OS updates reset grub.conf.  Re-run this USB package after
    any official firmware update to restore the grub hook.

Scenario 2 -- Root-hacked Kronos (OA.clonos.rc AND clontab present):
    posttar.sh injects a call to /korg/rw/kronosmods_init into OA.clonos.rc
    after the STATUS=$? line (i.e. after loadoa returns).  Clontab parallel
    entries can disrupt loadoa on the RTAI kernel; injection here is safe.

Scenario 3 -- Non-rooted Kronos with customised grub.conf:
    grub.conf exists but isn't factory-state (more than one entry, non-zero
    default, or already has an init= parameter).  posttar.sh copies the
    current default boot entry, adds  init=/korg/kronos_init  to the
    kernel line of the copy, appends it as a new "KronosMods Boot" entry,
    and updates 'default' to point to the new entry.  The original default
    is saved to /korg/rw/.grub_orig_default for the uninstaller.

Diagnostic: posttar.sh writes /korg/rw/HD/ScreenRemote/install_diag.txt immediately on
    start, then appends scenario and exit-code lines throughout.  Visible
    via FTP on the non-rooted Kronos.  If this file is absent after install,
    UpdateOS killed posttar before it ran (likely pretar.sh md5 failure).

All /korg/rw/ files (kronosmods_init, kronos_init, modules, binaries) survive
Korg OS updates.  grub.conf does not -- it is reset on every Korg firmware
update (scenarios 1 and 3 require re-running the USB package after updates).

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

# UpdateOS accepts any VERSION string — it only checks the field is present and
# does not compare it against the running firmware.  Kept here for reference only;
# install.info uses "{pkg_name} {version}" so the Kronos display shows the actual
# package identity rather than the firmware version number.
# Title used for the new grub entry in scenario 3.
GRUB_ENTRY_TITLE = "KronosMods Boot"

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

    Called by /korg/kronos_init (GRUB path, scenarios 1 & 3) after
    /korg/rw is mounted, or injected directly into OA.clonos.rc (scenario 2).
    /korg/rw is already mounted when this runs in both cases.
    """
    lines = [
        "#!/bin/sh",
        "# kronosmods_init -- KronosMods boot commands",
        "# Called from /korg/kronos_init (GRUB) or OA.clonos.rc (rooted).",
        "# /korg/rw is already mounted when this script runs.",
        "",
    ]
    if debug_log:
        import posixpath
        log_dir = posixpath.dirname(debug_log)
        lines += [
            f"mkdir -p {log_dir} 2>/dev/null || true",
            f"exec >> {debug_log} 2>&1",
            "echo 'kronosmods_init: starting'",
            "",
        ]
    if boot_cmds:
        for cmd in boot_cmds:
            lines.append(cmd)
            if debug_log:
                label = cmd.strip().split()[0] if cmd.strip() else "cmd"
                lines.append(f"echo \"kronosmods_init: {label} exited $?\"")
        lines.append("")
    if debug_log:
        lines += [
            "echo 'kronosmods_init: done'",
            f"chown -R 500:500 {log_dir} 2>/dev/null || true",
            "",
        ]
    return "\n".join(lines)


def _make_kronos_init(debug_log: str = "") -> str:
    """
    Shell script installed at /korg/kronos_init (root filesystem, sda2).

    Used as the GRUB init= target for scenarios 1 and 3 (non-rooted Kronos).
    MUST live on sda2 (the root fs) -- /korg/rw (sda6) is not yet mounted when
    the kernel execs the init= target, so pointing init= at /korg/rw/... would
    produce ENOENT and silently fall back to /sbin/init with no error.

    Mounts /korg/rw, delegates to kronosmods_init, then exec-chains to /sbin/init.
    Boot-diagnostic log (grub.conf snapshot) is written only when debug_log is set.
    """
    import posixpath
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
    if debug_log:
        diag_dir = posixpath.dirname(debug_log)
        lines += [
            "# Write boot-diagnostic log to FTP path (readable after reboot via FTP).",
            f"mkdir -p {diag_dir} 2>/dev/null || true",
            f"{{ echo 'kronos_init: started'; cat /boot/grub/grub.conf; }} >> {diag_dir}/kronos_boot.log 2>/dev/null",
            "",
        ]
    lines += [
        "# Run boot setup (insmod modules, start daemons, etc.).",
        "[ -x /korg/rw/kronosmods_init ] && /korg/rw/kronosmods_init",
        "",
        "# Hand off to the real init (this process becomes PID 1).",
        "exec /sbin/init \"$@\"",
        "",
    ]
    return "\n".join(lines)


def _make_pretar(tarball_name: str, tarball_md5: str, pkg_name: str) -> str:
    return "\n".join([
        "#!/bin/sh",
        f"# pretar.sh -- {pkg_name}",
        "",
        "echo 'Verifying install media...' > /tmp/installer_status",
        "",
        "# Abort if tarball is corrupted",
        "checksum=`/mnt/updaterSource/md5sum \"/mnt/updaterSource/" + tarball_name
        + "\" | awk '{ print $1; }'`",
        "if [ \"" + tarball_md5 + "\" != \"$checksum\" ]; then",
        "    kill -9 $(/sbin/pidof UpdateOS)",
        "fi",
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
                  debug_log: str = "") -> str:
    lines = [
        "#!/bin/sh",
        f"# posttar.sh -- {pkg_name}",
        "",
    ]

    # Fixed FTP-accessible directory used for all install-time diagnostics.
    diag_dir = "/korg/rw/HD/ScreenRemote"

    def _diag(msg):
        """Echo a diagnostic line to install_diag.txt — omitted when not logging."""
        return [f"echo \"{msg}\" >> {diag_dir}/install_diag.txt 2>/dev/null"] if debug_log else []

    if boot_hook:
        if debug_log:
            lines += [
                f"mkdir -p {diag_dir} 2>/dev/null || true",
                f"echo \"posttar: started $(date)\" >> {diag_dir}/install_diag.txt 2>/dev/null",
                "sync 2>/dev/null",
                "",
            ]

        lines += [
            "chmod 755 /korg/rw/kronosmods_init /korg/kronos_init 2>/dev/null || true",
            "",
            "# -- Scenario 2: inject into OA.clonos.rc if root-hack files are present -------",
            "# Both OA.clonos.rc AND clontab must exist.  OA.clonos.rc alone can be a ghost",
            "# left by a prior root-hack after a Korg firmware reinstall -- clontab is the",
            "# actual caller (via /bin/init), so without it OA.clonos.rc is never invoked.",
            "# NOTE: even when Scenario 2 fires we still check grub.conf below, because a",
            "# Korg firmware update resets grub.conf to factory state (no init=/bin/init),",
            "# leaving the root-hack files present but inactive via GRUB.",
            "if [ -f /etc/OA.clonos.rc ] && [ -f /etc/clontab ]; then",
            *_diag("posttar: root-hack files present (OA.clonos.rc + clontab) - injecting"),
            "    if ! grep -q 'kronosmods' /etc/OA.clonos.rc; then",
            "        awk '/^STATUS=/{print; print \"/korg/rw/kronosmods_init\"; next}1' \\",
            "            /etc/OA.clonos.rc > /tmp/_rc_new \\",
            "            && mv /tmp/_rc_new /etc/OA.clonos.rc",
            "        chmod 755 /etc/OA.clonos.rc",
            "        sync",
            "    fi",
            "fi",
            "",
            "# -- Scenarios 1, 2 (ghost-grub), 3: mount sda1 and check/patch grub.conf -----",
            "# /boot (sda1) is never mounted when UpdateOS runs -- always mount it.",
            "# Skip patching only if the active default entry already uses the root-hack init",
            "# (init=/bin/init), meaning the root-hack GRUB entry is live and OA.clonos.rc",
            "# will be called at boot.  In all other cases (factory GRUB, ghost-rooted where",
            "# a Korg update reset grub.conf) we add init=/korg/kronos_init.",
            "_boot_mounted=0",
            "if mount -t vfat /dev/sda1 /boot 2>/dev/null; then",
            "    _boot_mounted=1",
            *_diag("posttar: mounted sda1 as vfat"),
            "elif mount /dev/sda1 /boot 2>/dev/null; then",
            "    _boot_mounted=1",
            *_diag("posttar: mounted sda1 (auto-type)"),
            "else",
            *_diag("posttar: WARNING: failed to mount /dev/sda1 at /boot"),
            "fi",
            "",
            "if [ -f /boot/grub/grub.conf ]; then",
            "    _DF=$(awk '/^default[[:space:]=]/{v=$0;sub(/^default[[:space:]=]*/,\"\",v);print v;exit}' /boot/grub/grub.conf)",
            "    : ${_DF:=0}",
            "    _KL=$(awk -v d=\"$_DF\" 'BEGIN{e=-1}/^title/{e++}e==d&&/^[[:space:]]*kernel /{print;exit}' /boot/grub/grub.conf)",
            *_diag("posttar: grub default=$_DF kernel_line=$_KL"),
            "",
            "    if echo \"$_KL\" | grep -q 'init=/bin/init'; then",
            "        # Root-hack GRUB entry is active -- OA.clonos.rc will be called at boot.",
            *_diag("posttar: root-hack GRUB active (init=/bin/init) - no grub patch needed"),
            "",
            "    else",
            "        # Factory GRUB (or ghost-rooted after Korg update) -- patch grub.conf.",
            *(
                [
                    f"        cp /boot/grub/grub.conf {diag_dir}/grub_preinstall.txt 2>/dev/null || true",
                    "        sync 2>/dev/null",
                ] if debug_log else []
            ),
            "        # Remove stale init= from any old package build that used the wrong path.",
            "        sed -i 's| init=/korg/rw/kronos_init||g' /boot/grub/grub.conf 2>/dev/null || true",
            "        if ! grep -q 'init=/korg/kronos_init' /boot/grub/grub.conf; then",
            "            _TC=$(awk '/^title /{n++} END{print n+0}' /boot/grub/grub.conf)",
            "            _HI=$(awk '/init=/{n++} END{print n+0}' /boot/grub/grub.conf)",
            *_diag("posttar: grub entries=$_TC init_lines=$_HI default=$_DF"),
            "",
            "            if [ \"$_TC\" -le 1 ] && [ \"$_HI\" = '0' ] && [ \"$_DF\" = '0' ]; then",
            "                # -- Scenario 1: Factory grub (single entry, default=0, no init=) ---",
            *_diag("posttar: scenario 1 (factory grub - patching kernel line)"),
            "                awk '/^[[:space:]]*kernel \\/bzImage /{print $0 \" init=/korg/kronos_init\"; next} {print}' \\",
            "                    /boot/grub/grub.conf > /tmp/_grub_new \\",
            "                    && mv /tmp/_grub_new /boot/grub/grub.conf",
            *_diag("posttar: scenario 1 patch exit=$?"),
            "",
            "            else",
            "                # -- Scenario 3: Custom grub (multiple entries / non-zero default) ---",
            *_diag("posttar: scenario 3 (custom grub - adding new entry)"),
            "                echo \"$_DF\" > /korg/rw/.grub_orig_default",
            "                _NI=$(awk '/^title /{n++} END{print n+0}' /boot/grub/grub.conf)",
            "                _RL=$(awk -v d=\"$_DF\" \\",
            "                    'BEGIN{e=-1}/^title/{e++}e==d&&/^[[:space:]]*root /{print;exit}' \\",
            "                    /boot/grub/grub.conf)",
            "                {",
            "                    awk -v nd=\"$_NI\" \\",
            "                        '/^default[[:space:]=]/{print \"default \" nd; next} {print}' \\",
            "                        /boot/grub/grub.conf",
            f"                    printf '\\ntitle {GRUB_ENTRY_TITLE}\\n'",
            "                    printf '%s\\n' \"$_RL\"",
            "                    printf '%s init=/korg/kronos_init\\n' \"$_KL\"",
            "                } > /tmp/_grub_new && mv /tmp/_grub_new /boot/grub/grub.conf",
            *_diag("posttar: scenario 3 patch exit=$?"),
            "            fi",
            "        else",
            *_diag("posttar: init=/korg/kronos_init already present"),
            "        fi",
            *(
                [
                    f"        cp /boot/grub/grub.conf {diag_dir}/grub_postinstall.txt 2>/dev/null || true",
                    "        sync 2>/dev/null",
                ] if debug_log else []
            ),
            "    fi",
            "else",
            *_diag("posttar: WARNING: /boot/grub/grub.conf not found after mount attempt"),
            "    echo 'kronosmods: WARNING: no grub hook installed' > /dev/kmsg",
            "fi",
            "",
            "[ \"$_boot_mounted\" = '1' ] && umount /boot 2>/dev/null || true",
            "",
        ]
        if debug_log:
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
        "sync",
        "echo 'set 0' > /proc/OmapNKS4ProgressBar",
        "_p=10",
        "for _i in 1 2 3 4 5 6 7 8 9 10; do",
        "    echo \"set $_p\" > /proc/OmapNKS4ProgressBar",
        "    sleep 1",
        "    _p=$(( _p + 9 ))",
        "done",
        "echo 'set 100' > /proc/OmapNKS4ProgressBar",
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
            "# -- Remove boot hook (mirrors the three install scenarios) -------------------",
            "",
            "if [ -f /etc/OA.clonos.rc ] && [ -f /etc/clontab ]; then",
            "    # Scenario 2: remove injection from OA.clonos.rc",
            "    awk '!/kronosmods/' /etc/OA.clonos.rc > /tmp/_rc_new \\",
            "        && mv /tmp/_rc_new /etc/OA.clonos.rc",
            "    chmod 755 /etc/OA.clonos.rc",
            "    sync",
            "fi",
            "",
            "# Always mount sda1 to undo grub.conf changes (sda1 is never pre-mounted).",
            "_boot_mounted=0",
            "if mount -t vfat /dev/sda1 /boot 2>/dev/null; then",
            "    _boot_mounted=1",
            "elif mount /dev/sda1 /boot 2>/dev/null; then",
            "    _boot_mounted=1",
            "fi",
            "if [ -f /boot/grub/grub.conf ]; then",
            "    if [ -f /korg/rw/.grub_orig_default ]; then",
            "        # Scenario 3: remove the KronosMods Boot entry and restore default",
            "        _ORIG=$(cat /korg/rw/.grub_orig_default)",
            "        awk -v o=\"$_ORIG\" \\",
            f"        'BEGIN{{skip=0}}/^default[[:space:]=]/{{print \"default \" o; next}}/^title {GRUB_ENTRY_TITLE}$/{{skip=1;next}}skip&&/^title /{{skip=0;print;next}}!skip{{print}}' \\",
            "            /boot/grub/grub.conf > /tmp/_grub_new \\",
            "            && mv /tmp/_grub_new /boot/grub/grub.conf",
            "        rm -f /korg/rw/.grub_orig_default",
            "    else",
            "        # Scenario 1: remove init= from the kernel line",
            "        sed -i 's| init=/korg/kronos_init||g' /boot/grub/grub.conf",
            "    fi",
            "    sync",
            "fi",
            "[ \"$_boot_mounted\" = '1' ] && umount /boot 2>/dev/null || true",
            "",
            "# Remove FTP log directory.",
            "rm -rf /korg/rw/HD/ScreenRemote 2>/dev/null || true",
            "",
        ]

    if installed_files:
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

        if lone_files:
            lines += ["# Remove installed files"]
            for f in lone_files:
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
        "sync",
        "echo 'set 0' > /proc/OmapNKS4ProgressBar",
        "_p=10",
        "for _i in 1 2 3 4 5 6 7 8 9 10; do",
        "    echo \"set $_p\" > /proc/OmapNKS4ProgressBar",
        "    sleep 1",
        "    _p=$(( _p + 9 ))",
        "done",
        "echo 'set 100' > /proc/OmapNKS4ProgressBar",
        "",
        "exit 0",
        "",
    ]
    return "\n".join(lines)


def _build_uninstaller(pkg_name: str, version: str, installed_files: list,
                       boot_hook: bool, uninstall_cmds: list,
                       md5sum_src: Path = None) -> Path:
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
      *.ko anywhere              -> insmod at boot (modules first); rmmod at uninstall
      no extension, anywhere     -> ... & at boot; kill $(pidof ...) at uninstall

    Returns (boot_cmds, uninstall_cmds).
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
                # No extension → executable binary (covers /bin/foo and /screenremote/foo alike)
                daemons.append((kpath, fname))

    boot_cmds: list    = []
    uninstall_cmds: list = []

    for kpath, modname in sorted(mods):
        boot_cmds.append(f"insmod {kpath} 2>/dev/null || true")
        uninstall_cmds.append(f"rmmod {modname} 2>/dev/null || true")

    for kpath, name in sorted(daemons):
        boot_cmds.append(f"{kpath} &")
        uninstall_cmds.append(f"kill $(pidof {name}) 2>/dev/null || true")

    return boot_cmds, uninstall_cmds


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    print("=" * 52)
    print("  Kronos Package Builder")
    print("=" * 52)
    print()

    pkg_name = _prompt("Package name (e.g. ScreenRemote, VKeyboard)")
    if not pkg_name:
        sys.exit("Package name is required.")
    # Sanitize for filenames and shell scripts — spaces break shell word-splitting
    # in pretar.sh and UpdateOS's SOURCE= parsing.  Display name keeps original.
    pkg_name_id = pkg_name.replace(" ", "_").replace("/", "_")
    if pkg_name_id != pkg_name:
        print(f"  (filename ID: {pkg_name_id})")

    version = _prompt("Version", "1.0.0")

    payload_str = _prompt("Payload directory (files organised under mnt/ prefix)")
    payload_dir = Path(payload_str)
    if not payload_dir.is_dir():
        sys.exit(f"Error: '{payload_dir}' is not a directory.")

    _check_payload_licensing(payload_dir)

    # Auto-detect boot and uninstall commands from the payload contents.
    boot_cmds, uninstall_cmds = _auto_detect_commands(payload_dir)

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
        f"chmod 755 {cmd.strip()[:-2]} 2>/dev/null || true"
        for cmd in boot_cmds
        if cmd.strip().endswith(" &")
    ]

    pkg_id       = f"{pkg_name_id}_{version.replace('.', '_')}"
    tarball_name = f"{pkg_id}.tar.gz"
    out_dir      = Path(__file__).parent / "dist" / pkg_id
    out_dir.mkdir(parents=True, exist_ok=True)

    debug_log = ""
    if boot_hook:
        if _prompt_yn("Enable FTP boot logging? (writes daemon output to /korg/rw/HD/)", default=False):
            debug_log = _prompt("FTP log path for daemon output", "/korg/rw/HD/ScreenRemote/kronosmods_boot.log")

    extra_files: dict = {}
    if boot_hook:
        extra_files["mnt/korg/rw/kronosmods_init"] = (_make_kronosmods_init(boot_cmds, debug_log), 0o755)
        extra_files["mnt/korg/kronos_init"]         = (_make_kronos_init(debug_log),                0o755)

    tarball_path = out_dir / tarball_name
    print(f"\nBuilding {tarball_name} ...")
    _build_tarball(payload_dir, tarball_path, extra_files)
    tarball_md5 = _md5_file(tarball_path)
    print(f"  MD5: {tarball_md5}")

    pretar_text  = _make_pretar(tarball_name, tarball_md5, pkg_name)
    posttar_text = _make_posttar(pkg_name, install_cmds, boot_hook, debug_log)
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
    _build_uninstaller(pkg_name, version, installed_files, boot_hook, uninstall_cmds, md5sum_src)

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
        print("Boot hook (auto-detected at install time on the Kronos):")
        print("  Scenario 1 -- factory grub.conf (1 entry, default=0, no init=)")
        print("    -> appends  init=/korg/kronos_init  to the kernel line")
        print("  Scenario 2 -- root-hacked Kronos (OA.clonos.rc + clontab both present)")
        print("    -> injects kronosmods_init call into OA.clonos.rc after loadoa")
        print("  Scenario 3 -- non-rooted Kronos with custom grub.conf")
        print(f"    -> adds new '{GRUB_ENTRY_TITLE}' entry (copy of default + init=)")
        print()
        print("  /korg/kronos_init lives on sda2 (root fs) -- accessible at init= time.")
        print("  /korg/rw/ files (kronosmods_init, daemon) survive Korg OS updates.")
        print("  grub.conf is reset by Korg OS updates (scenarios 1 & 3).")
        print("  Re-run this USB package after any Korg firmware update.")
        print()
        print("  Diagnostics (always written at install and boot time):")
        print("  All logs written to /korg/rw/HD/ScreenRemote/ (FTP-accessible):")
        print("    install_diag.txt    -- install-time scenario + exit codes")
        print("    grub_preinstall.txt -- grub.conf before posttar.sh (install-time)")
        print("    grub_postinstall.txt-- grub.conf after posttar.sh  (install-time)")
        print("    kronos_boot.log     -- appended on every boot if GRUB hook ran")
        if debug_log:
            print(f"    kronosmods_boot.log -- daemon stdout/stderr, appended on every boot")


if __name__ == "__main__":
    main()
