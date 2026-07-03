#!/usr/bin/env python3
"""One-shot DEBUG build -- reads version from first argument.

Debug builds make the GRUB menu visible (timeout=5, hiddenmenu removed) so a bad
boot can be observed and a good entry selected by hand, and write FTP-readable
diagnostics to /korg/rw/HD/ScreenRemote/.  Ship build_auto.py (release) once a
version is validated.
"""
import sys, shutil
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from build_package import (
    _make_kronosmods_init, _make_kronos_init, _make_pretar, _make_posttar,
    _sha1_signature, _md5_file, _build_tarball, _build_uninstaller,
    _auto_detect_commands, _MD5SUM_PAYLOAD_REL, _DISPLAY_MSG_PAYLOAD_REL,
)

DEBUG       = True
pkg_name    = "ScreenRemote"
pkg_name_id = "ScreenRemote"
version     = sys.argv[1] if len(sys.argv) > 1 else "1.0.0"
debug_log   = "/korg/rw/HD/ScreenRemote/kronosmods_boot.log" if DEBUG else ""
payload_dir = Path(__file__).parent / "payload"

# kronosmods_init defers all of these behind a /proc/.oacmd sentinel and a
# background block (see _make_kronosmods_init), so no per-command wrapping here.
boot_cmds, uninstall_cmds, daemon_paths = _auto_detect_commands(payload_dir)
install_cmds = [
    "chmod 755 {} 2>/dev/null || true".format(path)
    for path in daemon_paths
]

# vkbd.ko / midi_inject.ko are embedded in the screenremote binary and loaded via
# init_module(2).  Do NOT rmmod them here: busybox rmmod reads /proc/modules, and
# reading /proc/modules OOPSES this kernel whenever OA is loaded — module_refcount
# faults on OA's per-cpu refptr because OA is brought up by loadmod's custom
# decrypting loader, not the standard module path (see the /proc/modules landmine
# note).  Instead the daemon, killed above by _auto_detect_commands' uninstall
# command, unloads both modules ITSELF via delete_module(2) on SIGTERM (which
# restores OA's patched .text and frees the trampolines) — no /proc/modules
# access.  Give it a moment to finish; any module still resident is cleared by the
# reboot that completes the uninstall.
uninstall_cmds += [
    "sleep 2",
]

pkg_id       = "{}_{}".format(pkg_name_id, version.replace(".", "_"))
tarball_name = "{}.tar.gz".format(pkg_id)
out_dir      = Path(__file__).parent / "dist" / pkg_id
out_dir.mkdir(parents=True, exist_ok=True)

extra_files = {
    "mnt/korg/rw/kronosmods_init": (_make_kronosmods_init(boot_cmds, debug_log), 0o755),
    "mnt/korg/kronos_init":        (_make_kronos_init(DEBUG),                      0o755),
}

tarball_path = out_dir / tarball_name
_build_tarball(payload_dir, tarball_path, extra_files)
tarball_md5  = _md5_file(tarball_path)

pretar_text  = _make_pretar(tarball_name, tarball_md5, pkg_name)
posttar_text = _make_posttar(pkg_name, install_cmds, True, DEBUG)
sig          = _sha1_signature(pretar_text.encode(), posttar_text.encode())

install_info = (
    "VERSION={} {}\n"
    "SOURCE={}\n"
    "PRETARSCRIPT=pretar.sh\n"
    "POSTTARSCRIPT=posttar.sh\n"
    "SIGNATURE={}\n"
).format(pkg_name, version, tarball_name, sig)

(out_dir / "pretar.sh").write_text(pretar_text,  encoding="ascii", newline="\n")
(out_dir / "posttar.sh").write_text(posttar_text, encoding="ascii", newline="\n")
(out_dir / "install.info").write_text(install_info, encoding="ascii", newline="\n")
(out_dir / "mnt").mkdir(exist_ok=True)

md5sum_src = payload_dir.joinpath(*_MD5SUM_PAYLOAD_REL)
shutil.copy2(md5sum_src, out_dir / "md5sum")

display_msg_src = payload_dir.joinpath(*_DISPLAY_MSG_PAYLOAD_REL)
if display_msg_src.is_file():
    shutil.copy2(display_msg_src, out_dir / "DisplayUpdaterMessage")

installed_files = [
    "/korg/rw/screenremote/screenremote",
    "/korg/rw/kronosmods_init",
    "/korg/kronos_init",
]
_build_uninstaller(pkg_name, version, installed_files, True, uninstall_cmds,
                   md5sum_src, display_msg_src)

print("Signature: {}".format(sig))
print("Built: dist/{}/".format(pkg_id))
