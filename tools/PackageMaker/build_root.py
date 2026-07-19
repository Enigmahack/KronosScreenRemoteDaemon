#!/usr/bin/env python3
"""
build_root.py -- DevRoot: robust Kronos rooting + on-device developer environment.

This is the "root builder" sibling of build_package.py (ScreenRemote daemon) and
build_unroot.py.  Where the ScreenRemote installer only *hooks* the factory boot
(init=/korg/kronos_init on a factory-shaped grub entry) and leaves the userland
alone, DevRoot performs the full clonos substitution -- the same thing the legacy
KronosRootHack/ package does -- but reproducibly, idempotently, and without the
timeout=0 brick risk:

  * boots init=/bin/init (busybox) via a SECOND grub entry (factory entry kept as
    a selectable fallback), with timeout=3 and hiddenmenu removed so a bad boot is
    always recoverable from the menu;
  * installs busybox (/sbin/busybox) + a full applet symlink farm in /usr/bin so
    tail/head/dmesg/less/find/top/... are bare commands everywhere.  The farm goes
    in /usr/bin (empty on a factory unit) and PATH keeps /bin first, so the factory
    GNU userland (bash, sed, grep, gawk, tar, gzip) is never shadowed -- DevRoot
    only fills the gaps;
  * dropbear SSH with PERSISTENT host keys on /korg/rw (survive reboots and Korg OS
    updates), respawned by inittab;
  * Tier-2 developer/debug binaries (whatever is staged under
    payload_root/mnt/korg/rw/devroot/bin) exposed via /usr/bin symlinks;
  * emits a matching Uninstaller that normalises grub back toward factory shape
    (drops the init=/bin/init entry, restores timeout=0 + hiddenmenu + default)
    and only then, GRUB_OK-gated, removes the boot-critical /bin/init & /sbin/busybox.

Boot-chain / safety rules (CLAUDE.md + kronos_factory_rootfs memory), all honoured
below:
  * a genuinely rooted boot uses init=/bin/init DIRECTLY (never /korg/kronos_init);
  * ship ONLY inittab.busybox, NEVER /etc/inittab (busybox init dies parsing the
    factory sysvinit inittab: "Bad inittab entry at line 1, 8, 9");
  * every grub.conf / OA.clonos.* rewrite uses a SAME-DIRECTORY temp file, never
    /tmp (/boot is a separate vfat partition; a cross-fs mv is not atomic);
  * the boot-critical init target is only removed once grub.conf is verified to no
    longer reference it (GRUB_OK);
  * grub is patched IN PLACE by mounting sda1 in posttar -- never shipped as a
    wholesale grub.conf in the tarball (the tarball's mnt/boot would land on the
    sda2 mountpoint stub, not the real sda1 grub).

Usage:
    python3 build_root.py [VERSION] [--verbose-boot] [--authorized-keys FILE]

    VERSION            package version (default 1.0.0)
    --verbose-boot     DevRoot grub entry boots loglevel=7, no 'fastboot' (watch
                       the framebuffer console).  Default keeps factory-quiet boot.
    --authorized-keys  install this pubkey file to /root/.ssh/authorized_keys (0600)
                       for key-based SSH.  Without it, set a root password on first
                       login (passwd).
"""
import argparse
import io
import os
import shutil
import sys
import tarfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from build_package import (          # reuse the proven low-level helpers
    _sha1_signature, _md5_file, _make_pretar,
    _DUM, _MD5SUM_PAYLOAD_REL, _DISPLAY_MSG_PAYLOAD_REL,
    GRUB_DROP_ROOTHACK_ENTRY,
)

# busybox applets we deliberately do NOT put in the /usr/bin farm.
#  * init/linuxrc  -- PID-1 machinery; /bin/init is managed explicitly below.
#  * sh/bash/hush/ash -- never let busybox's ash shadow the factory GNU bash.
#  * login         -- provided as /bin/login explicitly (getty path).
#  * halt/poweroff/reboot/shutdown -- factory ELF versions live in /sbin (earlier
#    on PATH); keep the farm from offering a second, divergent implementation.
_FARM_EXCLUDE = {
    "init", "linuxrc", "sh", "bash", "hush", "ash", "login",
    "halt", "poweroff", "reboot", "shutdown",
}

# DevRoot grub entry title (also used by the uninstaller's drop logic, which keys
# off the init=/bin/init parameter, not the title -- so the title is cosmetic).
DEVROOT_TITLE = "Kronos DevRoot (SSH + busybox)"

# Persistent state lives here (survives reboots and Korg OS updates -- /korg/rw is
# not touched by a firmware update; only grub.conf and the sda2 rootfs are).
DEVROOT_DIR   = "/korg/rw/devroot"
DROPBEAR_DIR  = DEVROOT_DIR + "/dropbear"


# ---------------------------------------------------------------------------
# Config-file generators (installed via the tarball overlay)
# ---------------------------------------------------------------------------

def _make_inittab(log_to_hd: bool = False) -> str:
    """
    /etc/inittab.busybox -- busybox init table for the rooted boot.

    NOT named /etc/inittab: a Korg OS update restores the factory sysvinit
    /etc/inittab, and busybox init must never parse that (it dies on the sysvinit
    initdefault/powerfail entries).  Keeping our table under a distinct name means
    the two never collide.

    SSH host keys persist across reboots AND Korg OS updates via a symlink:
    /etc/dropbear -> /korg/rw/devroot/dropbear (created in the tarball).  dropbear
    -R creates any missing hostkey at its default /etc/dropbear/ path, which
    follows the symlink onto /korg/rw -- so keys are auto-generated once, reused
    forever, and never a fatal "missing keyfile" (unlike a static -r path).  A
    firmware update wipes the sda2 symlink but leaves the keys on /korg/rw; a
    re-root restores the symlink and the same keys come back.

    log_to_hd: noisy builds write syslog to /korg/rw/HD/DevRoot/messages, which is
    reachable over the Kronos's own vsftpd (FTP only serves /korg/rw/HD) -- so boot
    logs can be retrieved even if SSH never comes up.  Quiet builds log to
    /korg/rw/devroot/messages (SSH-only).
    """
    syslog_file = "/korg/rw/HD/DevRoot/messages" if log_to_hd else "/korg/rw/devroot/messages"
    return "\n".join([
        "# inittab.busybox -- DevRoot (busybox init).  NOT /etc/inittab.",
        "::sysinit:/etc/OA.clonos.si",
        "::wait:/etc/OA.clonos.rc start",
        "# SSH, auto-respawned.  -R auto-creates missing hostkeys at /etc/dropbear/,",
        "# which is symlinked onto /korg/rw so the keys persist (see _make_clonos_si).",
        "::respawn:/bin/dropbear -F -R",
        "# System logging so dmesg-class output persists and is greppable over SSH",
        f"# (and FTP-retrievable when it lands under /korg/rw/HD).  File: {syslog_file}",
        "# NOTE: /usr/bin, not /sbin -- syslogd/klogd only exist via the applet farm",
        "# (the factory /sbin never shipped them; a /sbin path here 'can't run: No such",
        "# file or directory' on every respawn, forever).",
        f"::respawn:/usr/bin/syslogd -n -O {syslog_file} -s 512 -b 4",
        "::respawn:/usr/bin/klogd -n",
        "# Rescue shells on the local console (askfirst = press Enter to activate).",
        "::ctrlaltdel:/sbin/shutdown -t3 -r now",
        "tty1::askfirst:-/bin/bash",
        "tty2::askfirst:-/bin/bash",
        "tty3::askfirst:-/bin/bash",
        "tty4::askfirst:-/bin/bash",
        "",
    ])


def _make_clonos_si(log_to_hd: bool = False) -> str:
    """
    /etc/OA.clonos.si -- sysinit.  Based on the proven KronosRootHack OA.clonos.si
    (mounts, keymap, hwclock, remount-rw, /korg partitions), plus DevRoot additions:
    create the persistent dropbear key dir and generate host keys once.

    Runs FIRST (before any wait/respawn action), so /korg/rw and the host keys are
    ready before OA.clonos.rc (loadoa) or dropbear start.
    """
    return "\n".join([
        "#!/bin/bash",
        "# OA.clonos.si -- DevRoot sysinit (replaces factory rc.sysinit on the rooted boot).",
        "export PATH=/bin:/sbin:/usr/bin:/usr/sbin",
        "HOSTNAME=kronos",
        "",
        "if [ ! -e /proc/mounts ]; then",
        "    mount -n -t proc proc /proc",
        "    mount -n -t sysfs sys /sys >/dev/null 2>&1",
        "fi",
        "mount -n -t usbfs usbfs /proc/bus/usb >/dev/null 2>&1",
        "",
        "# Ethernet drivers (same set the factory brings up).",
        "for eth_driver in r8169 asix smsc7500; do modprobe $eth_driver >/dev/null 2>&1; done",
        "",
        "export TERM=linux",
        "setterm -term linux -cursor on -blank 0 >/dev/tty1 2>/dev/null",
        "/sbin/hwclock --hctosys --utc 2>/dev/null",
        "export TZ=UTC0",
        "hostname ${HOSTNAME}",
        "",
        "# Remount root read-write, then mount the Kronos data partitions.",
        "mount commit=1 -n -o remount,rw / 2>/dev/null",
        "mount -f / 2>/dev/null",
        "mount -f /proc >/dev/null 2>&1",
        "mount -f /sys >/dev/null 2>&1",
        "mount -t devpts devpts /dev/pts >/dev/null 2>&1",
        "mount -t ext2 -o ro /dev/sda5 /korg/ro 2>/dev/null",
        "mount -t ext3 -o commit=1,noatime /dev/sda6 /korg/rw 2>/dev/null",
        "swapon -a >/dev/null 2>&1",
        "",
        "# The factory /dev only ships tty0-tty2 (static nodes, no mdev/udevd on this",
        "# image) -- tty3/tty4 are missing, and inittab's askfirst lines for them would",
        "# otherwise fail to open their device and respawn-loop forever ('can't open",
        "# /dev/tty3: No such file or directory', once a second).  MUST run AFTER the",
        "# remount-rw above: root starts out mounted read-only, so an mknod attempted",
        "# any earlier fails silently (stderr redirected) and never actually creates",
        "# the node -- which is exactly why an earlier version of this fix never took.",
        "for _n in 1 2 3 4; do",
        "    [ -e /dev/tty$_n ] || mknod /dev/tty$_n c 4 $_n 2>/dev/null",
        "done",
        "",
        "# Stale-pidfile cleanup (ported from the proven legacy KronosRootHack sysinit;",
        "# dropped in an earlier rewrite here, which regressed it): a reboot that wasn't",
        "# a clean shutdown leaves /var/run/*.pid and /var/lock/* from the previous boot",
        "# behind, and OA.clonos.rc's messagebus/avahi-daemon startup then refuses to run",
        "# ('pid file exists') on EVERY boot after the first.  Clear it before anything",
        "# that checks a pidfile starts.",
        "for afile in /var/lock/* /var/run/*; do",
        "    if [ -d \"$afile\" ]; then",
        "        case \"$afile\" in",
        "            */news|*/mon) ;;",
        "            */sudo|*/dovecot|*/cups) rm -f \"$afile\"/*/* 2>/dev/null ;;",
        "            */vmware|*/samba|*/screen|*/cvs) rm -rf \"$afile\"/* 2>/dev/null ;;",
        "            *) rm -f \"$afile\"/* 2>/dev/null ;;",
        "        esac",
        "    else",
        "        rm -f \"$afile\" 2>/dev/null",
        "    fi",
        "done",
        "",
        "# -- DevRoot: persistent SSH host-key directory ------------------------------",
        "# /etc/dropbear is a symlink -> here (shipped in the tarball).  Ensure the",
        "# target dir exists on /korg/rw BEFORE the respawn'd dropbear -R runs, so its",
        "# auto-generated hostkeys land on the persistent partition (survive reboots",
        "# and Korg OS updates) instead of the volatile sda2 rootfs.",
        f"mkdir -p {DROPBEAR_DIR} 2>/dev/null",
        "",
    ] + ([
        "# Noisy build: syslog dir under the FTP-served /korg/rw/HD so boot logs are",
        "# retrievable over the Kronos's own vsftpd even if SSH never comes up.",
        "mkdir -p /korg/rw/HD/DevRoot 2>/dev/null",
        "chown 500:500 /korg/rw/HD/DevRoot 2>/dev/null || true",
        "",
    ] if log_to_hd else []) + [
        "rm -fr /tmp/* 2>/dev/null",
        "exit 0",
        "",
    ])


def _make_clonos_rc() -> str:
    """
    /etc/OA.clonos.rc -- runlevel start/stop.  Keeps the synth fully functional:
    runs /sbin/loadoa exactly as the factory rc does, then brings up the network
    daemons.  DevRoot adds nothing that can block or fail loadoa.

    The body of start() is deliberately flush-left (no indentation), matching
    the legacy KronosRootHack OA.clonos.rc byte-for-byte in that respect: other
    installers (build_package.py's ScreenRemote hook) detect a rooted Kronos by
    finding this exact file and inject a line via `awk '/^STATUS=/{...}'` --an
    indented `    STATUS=$?` doesn't match that anchor, so the injection runs,
    finds nothing, and silently no-ops (confirmed: ScreenRemoteDaemon installed
    fine but never started at boot until this was fixed).  Keep flush-left.
    """
    return "\n".join([
        "#!/bin/bash",
        "# OA.clonos.rc -- DevRoot runlevel script (audio path preserved: loadoa runs as normal).",
        "export PATH=/bin:/sbin:/usr/bin:/usr/sbin",
        "",
        "start() {",
        "/sbin/loadoa",
        "STATUS=$?",
        "echo \"loadoa exit status: $STATUS\"",
        "/sbin/UpdateRandomSeed.sh &",
        "if [ $STATUS != 0 ]; then",
        "    echo 'loadoa failed'",
        "    [ -x /bin/ShowReauthScreen ] && /bin/ShowReauthScreen $STATUS",
        "fi",
        "# Bring up plugged ethernet interfaces (after loadoa, as the factory does).",
        "ETH_INTERFACES=\"`cat /proc/net/dev | awk '{ print $1 }' | egrep '^(eth|wlan)' | cut -d: -f1`\"",
        "for IF in $ETH_INTERFACES; do",
        "    ifplugd -i $IF -fIW -u0 -d10 -r /sbin/ifplugd.lite.action &",
        "done",
        "[ -x /etc/init.d/messagebus ]   && /etc/init.d/messagebus start",
        "[ -x /etc/init.d/avahi-daemon ] && /etc/init.d/avahi-daemon start",
        "[ -x /sbin/vsftpd ] && /sbin/vsftpd /etc/vsftpd.conf &",
        "}",
        "",
        "case \"$1\" in",
        "    start) start ;;",
        "    stop)  : ;;",
        "    *)     start ;;",
        "esac",
        "exit 0",
        "",
    ])


def _make_profile() -> str:
    """
    /etc/profile -- login-shell environment.  /bin FIRST so factory GNU tools win;
    /usr/bin (applet farm) and the Tier-2 devroot bin fill the gaps.
    """
    return "\n".join([
        "# DevRoot login profile.",
        f"export PATH=/bin:/sbin:/usr/bin:/usr/sbin:{DEVROOT_DIR}/bin",
        "export PS1='\\u@\\h:\\w\\$ '",
        "export TERM=${TERM:-linux}",
        "export PAGER=less",
        "",
    ])


# ---------------------------------------------------------------------------
# Tarball assembly (files + symlinks, built programmatically -- no on-disk
# symlinks, so this works on the CIFS-mounted repo where symlinks can't live)
# ---------------------------------------------------------------------------

def _busybox_applets(busybox_path: Path) -> list:
    """Enumerate applets from the ACTUAL busybox binary via --list (its own truth,
    not a hand-maintained list).  The i386 static binary runs natively on the
    x86-64 build host, so this is a plain subprocess call."""
    import subprocess
    out = subprocess.run([str(busybox_path), "--list"],
                         capture_output=True, text=True, check=True).stdout
    return sorted(a for a in out.split() if a and a not in _FARM_EXCLUDE)


def _add_file(tar: tarfile.TarFile, arcname: str, data: bytes, mode: int) -> None:
    ti = tarfile.TarInfo(name=arcname)
    ti.size = len(data)
    ti.mode = mode
    ti.uid = ti.gid = 0
    tar.addfile(ti, io.BytesIO(data))


def _add_symlink(tar: tarfile.TarFile, arcname: str, target: str) -> None:
    ti = tarfile.TarInfo(name=arcname)
    ti.type = tarfile.SYMTYPE
    ti.linkname = target
    ti.mode = 0o777
    ti.uid = ti.gid = 0
    tar.addfile(ti)


def _add_dir(tar: tarfile.TarFile, arcname: str, mode: int) -> None:
    ti = tarfile.TarInfo(name=arcname)
    ti.type = tarfile.DIRTYPE
    ti.mode = mode
    ti.uid = ti.gid = 0
    tar.addfile(ti)


def _build_devroot_tarball(payload_root: Path, out_path: Path,
                           authorized_keys: bytes = None,
                           log_to_hd: bool = False) -> list:
    """
    Assemble the DevRoot tarball.  Returns the list of installed absolute paths
    (for the uninstaller's file-removal manifest).

    Layout laid over / (from tarball mnt/):
      /sbin/busybox                       real binary (staged)
      /bin/dropbear|dropbearkey|scp|nano  real binaries (staged)
      /bin/init  -> ../sbin/busybox       busybox PID-1 (init= target)
      /bin/login -> ../sbin/busybox       getty login
      /usr/bin/<applet> -> ../../sbin/busybox   applet farm (gap-fill)
      /korg/rw/devroot/bin/<tool>         Tier-2 binaries (staged)
      /usr/bin/<tool> -> /korg/rw/devroot/bin/<tool>   Tier-2 on PATH
      /etc/inittab.busybox, OA.clonos.si, OA.clonos.rc, profile   generated
      /root/.ssh/authorized_keys          only if --authorized-keys given
    """
    installed: list = []
    busybox_src = payload_root / "mnt" / "sbin" / "busybox"
    if not busybox_src.is_file():
        sys.exit(f"Error: staged busybox not found at {busybox_src}")

    applets = _busybox_applets(busybox_src)

    with tarfile.open(out_path, "w:gz") as tar:
        # --- real staged binaries under mnt/ (busybox, dropbear, e2fsck, ...) ----
        tier2_names = []
        for root, _dirs, files in os.walk(payload_root / "mnt"):
            for fname in sorted(files):
                fpath   = Path(root) / fname
                arcname = fpath.relative_to(payload_root).as_posix()   # mnt/...
                kpath   = "/" + arcname[len("mnt/"):]                  # /...
                _add_file(tar, arcname, fpath.read_bytes(), 0o755)
                installed.append(kpath)
                if kpath.startswith(DEVROOT_DIR + "/bin/"):
                    tier2_names.append(kpath.rsplit("/", 1)[-1])

        # --- PID-1 + login symlinks (busybox lives at /sbin/busybox) -------------
        _add_symlink(tar, "mnt/bin/init",  "../sbin/busybox")
        _add_symlink(tar, "mnt/bin/login", "../sbin/busybox")
        installed += ["/bin/init", "/bin/login"]

        # --- applet farm in /usr/bin (empty on factory; /bin stays ahead on PATH) -
        for app in applets:
            _add_symlink(tar, f"mnt/usr/bin/{app}", "../../sbin/busybox")
            installed.append(f"/usr/bin/{app}")

        # --- Tier-2 tools exposed on PATH via /usr/bin symlinks ------------------
        for name in sorted(tier2_names):
            if name in applets:
                continue  # farm already provides this name
            _add_symlink(tar, f"mnt/usr/bin/{name}", f"{DEVROOT_DIR}/bin/{name}")
            installed.append(f"/usr/bin/{name}")

        # --- persistent SSH host-key dir: /etc/dropbear -> /korg/rw/devroot/dropbear
        # dropbear -R creates missing keys at /etc/dropbear/, which follows this
        # symlink onto /korg/rw so they persist across reboots and OS updates.
        _add_symlink(tar, "mnt/etc/dropbear", DROPBEAR_DIR)
        installed.append("/etc/dropbear")

        # --- generated config files ---------------------------------------------
        for kpath, text, mode in [
            ("/etc/inittab.busybox", _make_inittab(log_to_hd),   0o644),
            ("/etc/OA.clonos.si",    _make_clonos_si(log_to_hd), 0o755),
            ("/etc/OA.clonos.rc",    _make_clonos_rc(),          0o755),
            ("/etc/profile",         _make_profile(),            0o644),
        ]:
            _add_file(tar, "mnt" + kpath, text.encode(), mode)
            installed.append(kpath)

        # --- optional authorized_keys -------------------------------------------
        # dropbear rejects authorized_keys unless ~/.ssh is 0700 and the key file
        # is not group/world-writable, all owned by the account -- ship both with
        # strict perms and root ownership.
        if authorized_keys:
            _add_dir(tar, "mnt/root/.ssh", 0o700)
            _add_file(tar, "mnt/root/.ssh/authorized_keys", authorized_keys, 0o600)
            installed.append("/root/.ssh/authorized_keys")

    return installed


# ---------------------------------------------------------------------------
# posttar (installer) -- grub in-place 2-entry transform
# ---------------------------------------------------------------------------

# awk program (POSIX; runs under factory gawk AND busybox awk) that transforms a
# factory-shaped grub.conf into a DevRoot one, atomically and idempotently:
#   * keep every existing entry verbatim (factory entry stays a fallback);
#   * append ONE new entry = a copy of the current default entry with the title
#     replaced and ' init=/bin/init' added to its kernel line;
#   * point default= at that new (last) entry;
#   * set timeout=3 and drop hiddenmenu so a bad boot is recoverable from the menu.
# The shell guards this with a grep for init=/bin/init so re-running is a no-op.
def _grub_transform_awk(title: str, verbose_boot: bool,
                        grub_path: str = "/boot/grub/grub.conf",
                        default_factory: bool = False) -> str:
    """
    grub_path      : the grub.conf to rewrite (/boot/... for UpdateOS posttar,
                     /mnt/boot/... for the rescue-USB apply path).
    default_factory: if True, leave `default` pointing at the factory entry
                     (auto-boots the known-good OS; DevRoot is opt-in from the
                     menu).  If False, `default` points at the appended DevRoot
                     entry (auto-boots root; factory is the manual fallback).
    """
    grub_dir = grub_path.rsplit("/", 1)[0]
    tmp_path = grub_dir + "/.grub_tmp"
    # which index `default=` gets: the appended DevRoot entry (ne) or the
    # original factory default (dflt).
    default_stmt = 'print "default=" dflt;' if default_factory else 'print "default=" ne;'
    # kernel-line mutation applied to the copied entry.  STRIP any pre-existing
    # init= first (e.g. a ScreenRemote init=/korg/kronos_init hook on the entry we
    # copy) so the DevRoot entry carries exactly one, unambiguous init=/bin/init --
    # a doubled `init=A init=B` boots B (last wins) but is fragile and confusing.
    kern = 'gsub(/ +init=[^ ]+/,"",s); s=s" init=/bin/init"'
    if verbose_boot:
        kern += '; sub(/loglevel=[0-9]+/,"loglevel=7",s); gsub(/ fastboot/,"",s)'
    return (
        "awk -v T='" + title + "' '"
        "BEGIN{inentry=0; ne=0; nh=0; dflt=0}"
        "/^[[:space:]]*title /{inentry=1; ne++; entry[ne]=$0 ORS; next}"
        "{"
        "  if(inentry){entry[ne]=entry[ne] $0 ORS; next}"
        "  if($0 ~ /^[[:space:]]*default=/){d=$0; sub(/^[[:space:]]*default=/,\"\",d); dflt=d+0; next}"
        "  if($0 ~ /^[[:space:]]*timeout=/){next}"
        "  if($0 ~ /^[[:space:]]*hiddenmenu/){next}"
        "  header[++nh]=$0"
        "}"
        "END{"
        "  di=dflt+1; if(di<1||di>ne) di=1;"
        "  n=split(entry[di], L, ORS); dev=\"\";"
        "  for(i=1;i<=n;i++){s=L[i]; if(s==\"\") continue;"
        "    if(s ~ /^[[:space:]]*title /){s=\"title \" T}"
        "    else if(s ~ /^[[:space:]]*kernel /){" + kern + "}"
        "    dev=dev s ORS}"
        "  for(i=1;i<=nh;i++) print header[i];"
        "  " + default_stmt +
        "  print \"timeout=3\";"
        "  for(i=1;i<=ne;i++) printf \"%s\", entry[i];"
        "  printf \"%s\", dev"
        "}' " + grub_path + " > " + tmp_path + " 2>/dev/null "
        "&& mv " + tmp_path + " " + grub_path
    )


def _make_posttar(pkg_name: str, verbose_boot: bool,
                  default_factory: bool = False) -> str:
    grub_awk = _grub_transform_awk(DEVROOT_TITLE, verbose_boot,
                                   default_factory=default_factory)
    return "\n".join([
        "#!/bin/sh",
        f"# posttar.sh -- {pkg_name} (DevRoot installer)",
        "",
        "chmod 755 /sbin/busybox /bin/init /bin/login 2>/dev/null || true",
        "chmod 755 /etc/OA.clonos.si /etc/OA.clonos.rc 2>/dev/null || true",
        "mknod /dev/tty c 5 0 2>/dev/null || true",
        "",
        "# /etc/dropbear must be our symlink to the persistent key dir.  UpdateOS's tar",
        "# (like busybox's) can't replace an existing /etc/dropbear DIRECTORY with the",
        "# tarball's symlink, so fix it up here: back up any real dir and recreate the",
        "# link.  Without this, dropbear -R would write keys to a real /etc/dropbear on",
        "# sda2 (wiped by every OS update) instead of the persistent /korg/rw location.",
        "if [ -e /etc/dropbear ] && [ ! -L /etc/dropbear ]; then",
        "    rm -rf /etc/dropbear.pre-devroot",
        "    mv /etc/dropbear /etc/dropbear.pre-devroot",
        "    ln -s /korg/rw/devroot/dropbear /etc/dropbear",
        "fi",
        "",
        "# -- GRUB: add a second entry that boots init=/bin/init (busybox) ------------",
        "# /boot (sda1, vfat) is never mounted during UpdateOS -- mount it ourselves.",
        "# We KEEP the factory entry as a selectable fallback and set timeout=3 (menu",
        "# visible) so a bad DevRoot boot is always recoverable -- unlike the legacy",
        "# root hack's timeout=0, which left no recovery window.",
        "_boot_mounted=0",
        "if mount -t vfat /dev/sda1 /boot 2>/dev/null; then _boot_mounted=1",
        "elif mount /dev/sda1 /boot 2>/dev/null; then _boot_mounted=1",
        "fi",
        "",
        "if [ -f /boot/grub/grub.conf ]; then",
        "    if grep -q 'init=/bin/init' /boot/grub/grub.conf 2>/dev/null; then",
        "        # Already DevRooted -- do not append a second copy (idempotent).",
        "        :",
        "    else",
        "        # Same-directory temp file (/boot/grub/.grub_tmp), NOT /tmp: /boot is a",
        "        # separate vfat partition, so a /tmp temp would make the mv a",
        "        # cross-filesystem copy+unlink instead of an atomic rename(2) -- a power",
        "        # loss mid-write could truncate grub.conf and brick the boot.",
        "        " + grub_awk,
        "        sync",
        "    fi",
        "else",
        "    echo 'DevRoot: WARNING: /boot/grub/grub.conf not found' > /dev/kmsg 2>/dev/null || true",
        "fi",
        "[ \"$_boot_mounted\" = '1' ] && umount /boot 2>/dev/null || true",
        "",
        "sync",
        "",
        # progress bar / display feedback (same UX as the ScreenRemote installer)
        f"{_DUM} 'Verifying installation...' 2>/dev/null",
        f"{_DUM} 'SetDefaultPalette' 2>/dev/null",
        "echo 'set 0' > /proc/OmapNKS4ProgressBar 2>/dev/null",
        "_p=10",
        "for _i in 1 2 3 4 5 6 7 8 9 10; do",
        "    echo \"set $_p\" > /proc/OmapNKS4ProgressBar 2>/dev/null",
        "    sleep 1",
        "    _p=$(( _p + 9 ))",
        "done",
        "echo 'set 100' > /proc/OmapNKS4ProgressBar 2>/dev/null",
        f"{_DUM} 'SetTextPalette' 2>/dev/null",
        "",
        "exit 0",
        "",
    ])


# ---------------------------------------------------------------------------
# apply_devroot.sh -- root the SSD from the KronosRescue LIVE environment
# (updater-independent path: use when Global > Update System Software fails)
# ---------------------------------------------------------------------------

def _make_apply_devroot(tarball_name: str, tarball_md5: str,
                        verbose_boot: bool, default_factory: bool) -> str:
    """
    Runs inside the KronosRescue "Live Rescue" environment, which has already
    mounted the SSD read-write at the on-device-mirrored paths:
        /mnt/root            = sda2 (Kronos rootfs)
        /mnt/boot            = sda1 (vfat, GRUB)      [also bind at /mnt/root/boot]
        /mnt/root/korg/rw    = sda6 (writable data)
    It performs the same install the UpdateOS posttar does, but against those
    mount points instead of the live root -- so it roots the unit with NO
    dependency on the Korg updater.

    busybox 1.23.2 tar has no --strip-components, so we extract the (mnt/-prefixed)
    tarball to a temp dir and `cp -a` its contents onto /mnt/root, which preserves
    the applet-farm symlinks, the /etc/dropbear symlink, and the strict /root/.ssh
    perms.  /korg/rw/... lands on the sda6 mount because it is mounted at
    /mnt/root/korg/rw.
    """
    grub_awk = _grub_transform_awk(DEVROOT_TITLE, verbose_boot,
                                   grub_path="/mnt/boot/grub/grub.conf",
                                   default_factory=default_factory)
    return "\n".join([
        "#!/bin/sh",
        "# apply_devroot.sh -- root the Kronos SSD from the Live Rescue USB.",
        "# Run this from the KronosRescue 'Live Rescue' shell (or over its SSH).",
        "# It needs the SSD already mounted by the rescue init at /mnt/root + /mnt/boot.",
        "set -e",
        "HERE=$(dirname \"$0\")",
        f"TARBALL=\"$HERE/{tarball_name}\"",
        "",
        "echo '== DevRoot rescue apply =='",
        "",
        "# -- Sanity: refuse to run unless the rescue mounts look right ---------------",
        "if [ ! -x /mnt/root/sbin/loadoa ]; then",
        "    echo 'ERROR: /mnt/root is not the Kronos rootfs (no /sbin/loadoa).' >&2",
        "    echo '       Boot the KronosRescue Live Rescue entry first (it mounts the SSD).' >&2",
        "    exit 1",
        "fi",
        "if [ ! -f /mnt/boot/grub/grub.conf ]; then",
        "    echo 'ERROR: /mnt/boot/grub/grub.conf not found (sda1 not mounted?).' >&2",
        "    exit 1",
        "fi",
        "if ! mountpoint -q /mnt/root/korg/rw 2>/dev/null; then",
        "    echo 'ERROR: /mnt/root/korg/rw (sda6) is not mounted -- persistent data would',",
        "    echo '       land on the rootfs.  Aborting.' >&2",
        "    exit 1",
        "fi",
        "[ -f \"$TARBALL\" ] || { echo \"ERROR: payload $TARBALL not found next to this script.\" >&2; exit 1; }",
        "",
        "# -- Verify payload integrity ------------------------------------------------",
        "sum=$(md5sum \"$TARBALL\" | awk '{print $1}')",
        f"if [ \"$sum\" != \"{tarball_md5}\" ]; then",
        "    echo \"ERROR: payload md5 mismatch (got $sum).\" >&2; exit 1",
        "fi",
        "",
        "# -- Extract to a temp dir, then cp -a onto the SSD --------------------------",
        "TMP=/tmp/devroot_apply",
        "rm -rf \"$TMP\"; mkdir -p \"$TMP\"",
        "tar -xzf \"$TARBALL\" -C \"$TMP\"        # creates $TMP/mnt/...",
        "# The tarball ships /etc/dropbear as a symlink to the persistent key dir.",
        "# busybox cp -a cannot replace an existing DIRECTORY with a symlink (a leftover",
        "# /etc/dropbear from an earlier root hack), so move any real one aside first.",
        "# An existing symlink is fine -- cp -a overwrites it in place.",
        "if [ -e /mnt/root/etc/dropbear ] && [ ! -L /mnt/root/etc/dropbear ]; then",
        "    rm -rf /mnt/root/etc/dropbear.pre-devroot",
        "    mv /mnt/root/etc/dropbear /mnt/root/etc/dropbear.pre-devroot",
        "    echo '  moved existing /etc/dropbear dir -> /etc/dropbear.pre-devroot'",
        "fi",
        "cp -a \"$TMP/mnt/.\" /mnt/root/          # merge tree (symlinks + perms preserved)",
        "rm -rf \"$TMP\"",
        "chmod 755 /mnt/root/sbin/busybox /mnt/root/bin/init /mnt/root/bin/login 2>/dev/null || true",
        "chmod 755 /mnt/root/etc/OA.clonos.si /mnt/root/etc/OA.clonos.rc 2>/dev/null || true",
        "mkdir -p /mnt/root/korg/rw/devroot/dropbear 2>/dev/null || true",
        "",
        "# -- GRUB: same 2-entry transform the posttar does, on the mounted sda1 ------",
        "if grep -q 'init=/bin/init' /mnt/boot/grub/grub.conf 2>/dev/null; then",
        "    echo 'grub.conf already has a DevRoot entry -- leaving it (idempotent).'",
        "else",
        "    " + grub_awk,
        "    echo 'grub.conf patched (DevRoot entry added).'",
        "fi",
        "sync",
        "",
        "echo '== Done.  Remove the rescue USB and reboot. =='",
        "echo '   Boot menu auto-selects "
        + ("FACTORY" if default_factory else "DevRoot")
        + " after 3s; pick the other entry to switch.'",
        "exit 0",
        "",
    ])


# ---------------------------------------------------------------------------
# posttar (uninstaller) -- normalise grub, GRUB_OK-gated removal of init target
# ---------------------------------------------------------------------------

def _make_uninstall_posttar(pkg_name: str, installed_files: list) -> str:
    # Boot-critical: removing these while grub.conf still execs one as init= would
    # strand the unit unbootable.  Gated behind GRUB_OK below.
    BOOT_CRITICAL = {"/bin/init", "/sbin/busybox"}

    lines = [
        "#!/bin/sh",
        f"# posttar.sh -- {pkg_name} (DevRoot uninstaller)",
        "",
        "# -- GRUB: drop the init=/bin/init entry, restore factory-shaped header -------",
        "_boot_mounted=0",
        "if mount -t vfat /dev/sda1 /boot 2>/dev/null; then _boot_mounted=1",
        "elif mount /dev/sda1 /boot 2>/dev/null; then _boot_mounted=1",
        "fi",
        "GRUB_OK=0",
        "if [ -f /boot/grub/grub.conf ]; then",
        "    # Remove the whole init=/bin/init entry (same helper the cleaner/unroot use).",
        "    " + GRUB_DROP_ROOTHACK_ENTRY,
        "    # Restore factory-quiet header: default first entry, no menu delay, hidden.",
        "    sed -i 's/^default=.*/default=0/'  /boot/grub/grub.conf 2>/dev/null || true",
        "    sed -i 's/^timeout=.*/timeout=0/'  /boot/grub/grub.conf 2>/dev/null || true",
        "    grep -q '^hiddenmenu' /boot/grub/grub.conf 2>/dev/null || \\",
        "        sed -i '0,/^timeout=/s/^timeout=.*/&\\nhiddenmenu/' /boot/grub/grub.conf 2>/dev/null || true",
        "    sync",
        "    if ! grep -q 'init=/bin/init' /boot/grub/grub.conf 2>/dev/null; then",
        "        GRUB_OK=1",
        "    fi",
        "fi",
        "[ \"$_boot_mounted\" = '1' ] && umount /boot 2>/dev/null || true",
        "",
    ]

    # Split installed files: boot-critical (gated) vs safe; collapse the applet
    # farm and devroot dir into bulk removals so the script stays small.
    safe   = [f for f in installed_files
              if f not in BOOT_CRITICAL
              and not f.startswith("/usr/bin/")
              and not f.startswith(DEVROOT_DIR + "/")]
    gated  = [f for f in installed_files if f in BOOT_CRITICAL]

    lines += [
        "# -- Remove the /usr/bin applet farm + Tier-2 symlinks (busybox/devroot links) -",
        "# Only remove symlinks that point at our busybox or the devroot tree, so any",
        "# unrelated /usr/bin entry a user added is left intact.",
        "if [ -d /usr/bin ]; then",
        "    for f in /usr/bin/*; do",
        "        [ -L \"$f\" ] || continue",
        "        t=`readlink \"$f\" 2>/dev/null`",
        "        case \"$t\" in",
        "            */sbin/busybox|/korg/rw/devroot/*) rm -f \"$f\" ;;",
        "        esac",
        "    done",
        "fi",
        "",
        "# -- Remove the persistent devroot tree (Tier-2 bins, host keys, logs) --------",
        f"rm -rf {DEVROOT_DIR} 2>/dev/null || true",
        "",
    ]

    if safe:
        lines += ["# -- Remove installed config files ------------------------------------------"]
        for f in sorted(safe):
            lines.append(f"rm -f {f}")
        lines += ["rmdir /root/.ssh 2>/dev/null || true", ""]

    if gated:
        lines += [
            "# -- Boot-critical: only remove once grub.conf no longer execs /bin/init -----",
            "if [ \"$GRUB_OK\" = '1' ]; then",
        ]
        for f in sorted(gated):
            lines.append(f"    rm -f {f}")
        lines += [
            "else",
            "    echo 'Partial uninstall: grub.conf init=/bin/init not confirmed removed --'"
            " \\",
            "         'left /bin/init + /sbin/busybox in place to avoid an unbootable init=' \\",
            "         > /korg/rw/DEVROOT_UNINSTALL_INCOMPLETE.txt 2>/dev/null || true",
            "fi",
            "",
        ]

    lines += [
        "sync",
        f"{_DUM} 'Verifying uninstall...' 2>/dev/null",
        f"{_DUM} 'SetDefaultPalette' 2>/dev/null",
        "echo 'set 0' > /proc/OmapNKS4ProgressBar 2>/dev/null",
        "_p=10",
        "for _i in 1 2 3 4 5 6 7 8 9 10; do",
        "    echo \"set $_p\" > /proc/OmapNKS4ProgressBar 2>/dev/null",
        "    sleep 1",
        "    _p=$(( _p + 9 ))",
        "done",
        "echo 'set 100' > /proc/OmapNKS4ProgressBar 2>/dev/null",
        f"{_DUM} 'SetTextPalette' 2>/dev/null",
        "",
        "exit 0",
        "",
    ]
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Package writers
# ---------------------------------------------------------------------------

def _write_package(out_dir: Path, pkg_name: str, version: str,
                   tarball_name: str, pretar: str, posttar: str,
                   md5sum_src: Path, display_msg_src: Path) -> str:
    sig = _sha1_signature(pretar.encode(), posttar.encode())
    info = (
        f"VERSION={pkg_name} {version}\n"
        f"SOURCE={tarball_name}\n"
        f"PRETARSCRIPT=pretar.sh\n"
        f"POSTTARSCRIPT=posttar.sh\n"
        f"SIGNATURE={sig}\n"
    )
    (out_dir / "pretar.sh").write_text(pretar,  encoding="ascii", newline="\n")
    (out_dir / "posttar.sh").write_text(posttar, encoding="ascii", newline="\n")
    (out_dir / "install.info").write_text(info,  encoding="ascii", newline="\n")
    (out_dir / "mnt").mkdir(exist_ok=True)
    if md5sum_src and md5sum_src.is_file():
        shutil.copy2(md5sum_src, out_dir / "md5sum")
    if display_msg_src and display_msg_src.is_file():
        shutil.copy2(display_msg_src, out_dir / "DisplayUpdaterMessage")
    return sig


def main() -> None:
    ap = argparse.ArgumentParser(prog="build_root.py",
                                 description="Build the DevRoot rooting + dev-env package.")
    ap.add_argument("version", nargs="?", default="1.0.0")
    ap.add_argument("--verbose-boot", action="store_true",
                    help="Noisy build: DevRoot grub entry boots loglevel=7 without "
                         "'fastboot', and syslog goes to the FTP-served "
                         "/korg/rw/HD/DevRoot/messages so boot logs are retrievable "
                         "even if SSH never comes up.")
    ap.add_argument("--authorized-keys", metavar="FILE",
                    help="install this pubkey to /root/.ssh/authorized_keys (0600).")
    ap.add_argument("--default-factory", action="store_true",
                    help="grub `default` stays on the FACTORY entry (auto-boots the "
                         "known-good OS; DevRoot is opt-in from the 3s menu).  Safer "
                         "for a first install.  Default: auto-boot DevRoot.")
    args = ap.parse_args()

    here         = Path(__file__).parent
    payload_root = here / "payload_root"
    payload_dir  = here / "payload"   # reuse ScreenRemote's md5sum + DisplayUpdaterMessage
    pkg_name     = "Kronos DevRoot"
    version      = args.version

    if not payload_root.is_dir():
        sys.exit(f"Error: {payload_root} not found (stage busybox + SSH binaries there).")

    ak_bytes = None
    if args.authorized_keys:
        ak_bytes = Path(args.authorized_keys).read_bytes()

    md5sum_src      = payload_dir.joinpath(*_MD5SUM_PAYLOAD_REL)
    display_msg_src = payload_dir.joinpath(*_DISPLAY_MSG_PAYLOAD_REL)

    # ---- installer ---------------------------------------------------------
    pkg_id       = f"Kronos_DevRoot_{version.replace('.', '_')}"
    out_dir      = here / "dist" / pkg_id
    out_dir.mkdir(parents=True, exist_ok=True)
    tarball_name = f"{pkg_id}.tar.gz"
    tarball_path = out_dir / tarball_name

    installed = _build_devroot_tarball(payload_root, tarball_path, ak_bytes,
                                       log_to_hd=args.verbose_boot)
    tarball_md5 = _md5_file(tarball_path)

    pretar  = _make_pretar(tarball_name, tarball_md5, pkg_name)
    posttar = _make_posttar(pkg_name, args.verbose_boot, args.default_factory)
    sig = _write_package(out_dir, pkg_name, version, tarball_name,
                         pretar, posttar, md5sum_src, display_msg_src)
    print(f"Installer:   dist/{pkg_id}/   (sig {sig[:16]}...)")
    print(f"  tarball:   {tarball_name}  ({tarball_path.stat().st_size} bytes, md5 {tarball_md5})")
    print(f"  installs:  {len(installed)} paths "
          f"({sum(1 for f in installed if f.startswith('/usr/bin/'))} applet-farm links)")
    print(f"  grub default: {'FACTORY (DevRoot opt-in)' if args.default_factory else 'DevRoot (factory fallback)'}"
          f"{', verbose/noisy boot' if args.verbose_boot else ''}")

    # ---- rescue-apply bundle (updater-independent rooting path) -------------
    # Use with the KronosRescue Live USB when Global > Update System Software
    # fails: boot rescue, get this bundle onto the unit, run apply_devroot.sh.
    rescue_dir = out_dir / "rescue"
    rescue_dir.mkdir(exist_ok=True)
    shutil.copy2(tarball_path, rescue_dir / tarball_name)
    apply_sh = _make_apply_devroot(tarball_name, tarball_md5,
                                   args.verbose_boot, args.default_factory)
    (rescue_dir / "apply_devroot.sh").write_text(apply_sh, encoding="ascii", newline="\n")
    (rescue_dir / "apply_devroot.sh").chmod(0o755)
    # KronosDoctor: all-in-one detect / diagnose / repair / fsck / (un)root tool,
    # auto-run at the rescue shell.  Rides along on the USB.
    for dsrc in ("kronos_doctor.sh", "kronos_boot_doctor.sh"):
        p = here / dsrc
        if p.is_file():
            shutil.copy2(p, rescue_dir / dsrc)
            (rescue_dir / dsrc).chmod(0o755)
    (rescue_dir / "README.txt").write_text(
        "DevRoot rescue-apply bundle -- root the Kronos WITHOUT the Korg updater.\n\n"
        "Use this when Global > Update System Software fails.\n\n"
        "1. Boot the KronosRescue USB and choose 'Live Rescue' (root shell + SSH;\n"
        "   it mounts the SSD at /mnt/root, /mnt/boot, /mnt/root/korg/rw).\n"
        "2. Get this whole 'rescue/' folder onto the unit, e.g. from 192.168.100.10:\n"
        "     scp -O -r rescue/ root@192.168.100.15:/tmp/devroot/\n"
        "   (Live Rescue SSH is passwordless; if DHCP fails it answers at 192.168.100.15.)\n"
        "3. Run it:\n"
        "     sh /tmp/devroot/apply_devroot.sh\n"
        "4. Remove the rescue USB and reboot.\n\n"
        "apply_devroot.sh verifies the payload md5, refuses to run unless the SSD is\n"
        "mounted correctly, extracts the same tarball the USB installer uses, and\n"
        "applies the identical grub 2-entry transform to /mnt/boot/grub/grub.conf.\n\n"
        "DIAGNOSE / RECOVER an existing install (any of factory / rooted /\n"
        "+ScreenRemote), without reinstalling:\n"
        "     sh kronos_boot_doctor.sh              # report the scenario + any problems\n"
        "     sh kronos_boot_doctor.sh --repair     # safely fix a broken/inconsistent grub\n",
        encoding="ascii", newline="\n")
    print(f"  rescue:    dist/{pkg_id}/rescue/  (apply_devroot.sh + payload — updater-independent root)")

    # ---- uninstaller -------------------------------------------------------
    upkg_id       = f"Kronos_DevRoot_{version.replace('.', '_')}_Uninstall"
    uout_dir      = here / "dist" / upkg_id
    uout_dir.mkdir(parents=True, exist_ok=True)
    utarball_name = f"{upkg_id}.tar.gz"
    with tarfile.open(uout_dir / utarball_name, "w:gz"):
        pass  # empty tarball -- an uninstaller only runs scripts
    utarball_md5 = _md5_file(uout_dir / utarball_name)

    upretar  = _make_pretar(utarball_name, utarball_md5, f"{pkg_name} Uninstaller")
    uposttar = _make_uninstall_posttar(f"{pkg_name} Uninstaller", installed)
    usig = _write_package(uout_dir, f"{pkg_name} Uninstall", version, utarball_name,
                          upretar, uposttar, md5sum_src, display_msg_src)
    print(f"Uninstaller: dist/{upkg_id}/   (sig {usig[:16]}...)")


if __name__ == "__main__":
    main()
