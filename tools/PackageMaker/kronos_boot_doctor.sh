#!/bin/sh
# kronos_boot_doctor.sh -- detect the Kronos boot scenario and (optionally) recover
# a boot that would otherwise fail.
#
# Four scenarios, from two INDEPENDENT booleans (ROOTED x SCREENREMOTE):
#     1  factory (unrooted)
#     2  rooted                       (DevRoot or legacy KronosRootHack)
#     3  factory + ScreenRemote
#     4  rooted + ScreenRemote
#
# Detection markers (read off the SSD's own filesystems, which survive even a
# corrupted grub.conf):
#   ROOTED       : grub has an init=/bin/init entry  OR  rootfs has
#                  /etc/inittab.busybox + /sbin/busybox
#   DEVROOT      : /korg/rw/devroot exists (else a rooted unit is the legacy hack)
#   SCREENREMOTE : /korg/kronos_init  OR  /korg/rw/screenremote/screenremote  OR
#                  /korg/rw/kronosmods_init  OR  grub has init=/korg/kronos_init
#
# Report-only by default.  With --repair it fixes the boot-preventing problems it
# can (atomic same-directory grub write; a boot-critical entry is dropped only once
# grub no longer references its init= target).  It fixes GRUB and simple rootfs
# permission/marker issues; it never installs software -- to (un)root use
# apply_devroot.sh / the DevRoot Uninstaller.
#
#   --repair CAN fix:
#     * grub.conf missing/empty            -> rebuild from a .bak, else a template
#     * DOS/CRLF line endings in grub.conf -> normalise to LF
#     * a doubled  init=A init=B           -> collapse to the one that should win
#     * init=/korg/kronos_init target gone -> strip it (entry boots factory /sbin/init)
#     * init=/bin/init busybox gone        -> drop that entry (unit boots factory)
#     * rooted rootfs, grub lost its entry -> REBUILD the init=/bin/init entry
#     * ScreenRemote installed, hook gone  -> RESTORE init=/korg/kronos_init on factory entry
#     * init target present but not +x     -> chmod 755 it (EACCES would fail PID 1)
#     * default= out of range / on a dead entry -> point it at a bootable entry
#     * rooted blind menu (timeout=0+hidden)    -> timeout>=3, no hiddenmenu (recoverable)
#   --repair CANNOT fix (reports only -- needs a reinstall/restore):
#     * /boot/bzImage (the kernel) missing        -> use autofix/Restore, or apply_devroot
#     * /etc/inittab.busybox missing on a rooted unit -> re-run apply_devroot
#
# Rescue use (SSD mounted by KronosRescue):  sh kronos_boot_doctor.sh [--repair]
# On the live device:                        sh kronos_boot_doctor.sh --on-device [--repair]

ROOT=/mnt/root          # sda2 rootfs   (rescue mount point)
BOOT=/mnt/boot          # sda1 grub     (rescue mount point)
REPAIR=0

# Last-resort factory templates, used ONLY to reconstruct a missing/empty
# grub.conf when no existing entry and no backup can supply the real kernel line.
# Derived from a real Kronos 1 factory grub.conf; memmap= is model-specific, so a
# reconstruct always prefers a real entry or a .bak over this template.
CANON_ROOT='	root (hd0,0)'
CANON_KERNEL='	kernel /bzImage root=/dev/sda2 max_loop=16 fbcon=map:0 memmap=384m vga=0x0303 loglevel=0 fastboot Single raid=noautodetect elevator=noop'

while [ $# -gt 0 ]; do
    case "$1" in
        --on-device) ROOT=/ ; BOOT=/boot ;;
        --root)      ROOT="$2"; shift ;;
        --boot)      BOOT="$2"; shift ;;
        --repair)    REPAIR=1 ;;
        -h|--help)   sed -n '2,55p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
    shift
done
[ "$ROOT" = "/" ] && ROOT=""
GRUB="$BOOT/grub/grub.conf"

say()  { echo "$*"; }
have() { [ -e "$1" ]; }
execu(){ [ -x "$1" ]; }             # exists AND executable

# ---------------------------------------------------------------------------
# Detect scenario
# ---------------------------------------------------------------------------
ROOTED=0; SR=0; DEVROOT=0
[ -f "$GRUB" ] && grep -q 'init=/bin/init' "$GRUB" 2>/dev/null && ROOTED=1
{ [ -f "$ROOT/etc/inittab.busybox" ] && have "$ROOT/sbin/busybox"; } && ROOTED=1
have "$ROOT/korg/rw/devroot" && DEVROOT=1
have "$ROOT/korg/kronos_init"                  && SR=1
have "$ROOT/korg/rw/screenremote/screenremote" && SR=1
have "$ROOT/korg/rw/kronosmods_init"           && SR=1
[ -f "$GRUB" ] && grep -q 'init=/korg/kronos_init' "$GRUB" 2>/dev/null && SR=1

if   [ $ROOTED = 1 ] && [ $SR = 1 ]; then SCEN=4; NAME="rooted + ScreenRemote"
elif [ $ROOTED = 1 ];                then SCEN=2; NAME="rooted"
elif [ $SR = 1 ];                    then SCEN=3; NAME="factory + ScreenRemote"
else                                      SCEN=1; NAME="factory (unrooted)"
fi
ROOTKIND=""
[ $ROOTED = 1 ] && { [ $DEVROOT = 1 ] && ROOTKIND="DevRoot" || ROOTKIND="legacy KronosRootHack"; }

# Capability flags used by both diagnostics and repair
ROOT_OK=0; { have "$ROOT/bin/init" && have "$ROOT/sbin/busybox"; } && ROOT_OK=1
SR_OK=0;   have "$ROOT/korg/kronos_init" && SR_OK=1
KERNEL_OK=0; have "$BOOT/bzImage" && KERNEL_OK=1

say "================ Kronos Boot Doctor ================"
say "  root fs (sda2): ${ROOT:-/}      grub (sda1): $GRUB"
[ -f "$GRUB" ] || say "  !! grub.conf not found -- is sda1 mounted at $BOOT ?"
say ""
say "  SCENARIO $SCEN: $NAME${ROOTKIND:+  [$ROOTKIND]}"
say ""
say "  markers:"
say "    ROOTED       = $ROOTED   (grub init=/bin/init: $([ -f "$GRUB" ] && grep -q 'init=/bin/init' "$GRUB" && echo yes || echo no); inittab.busybox: $(have "$ROOT/etc/inittab.busybox" && echo yes || echo no); busybox: $(have "$ROOT/sbin/busybox" && echo yes || echo no))"
say "    SCREENREMOTE = $SR   (kronos_init: $(have "$ROOT/korg/kronos_init" && echo yes || echo no); screenremote: $(have "$ROOT/korg/rw/screenremote/screenremote" && echo yes || echo no))"

# ---------------------------------------------------------------------------
# Consistency diagnostics -- catch the brick-class mismatches
# ---------------------------------------------------------------------------
say ""
say "  diagnostics:"
PROBLEMS=0; FATAL=0
warn()  { PROBLEMS=$((PROBLEMS+1)); say "    [!] $*"; }
fatal() { PROBLEMS=$((PROBLEMS+1)); FATAL=$((FATAL+1)); say "    [XX] $*"; }
ok()    { say "    [ok] $*"; }

# rootfs-level checks (independent of grub)
if [ ! -f "$ROOT/etc/inittab.busybox" ] && [ $ROOTED = 1 ] && have "$ROOT/sbin/busybox"; then
    warn "rooted unit but /etc/inittab.busybox is MISSING -> busybox init has no config (no SSH/loadoa). Re-run apply_devroot."
fi
for t in "$ROOT/bin/init" "$ROOT/korg/kronos_init"; do
    if have "$t" && ! execu "$t"; then warn "init target $t exists but is NOT executable -> PID 1 exec would fail (repairable: chmod +x)"; fi
done
have "$ROOT/sbin/busybox" && ! execu "$ROOT/sbin/busybox" && warn "/sbin/busybox is not executable (repairable: chmod +x)"

if [ ! -f "$GRUB" ] || ! grep -q '[^[:space:]]' "$GRUB" 2>/dev/null; then
    fatal "grub.conf is missing or empty -> GRUB has no menu to boot (repairable: rebuild)"
else
    NENT=$(grep -c '^[[:space:]]*title ' "$GRUB")
    DEF=$(awk -F= '/^[[:space:]]*default=/{print $2+0; exit}' "$GRUB"); DEF=${DEF:-0}
    TMO=$(awk -F= '/^[[:space:]]*timeout=/{print $2+0; exit}' "$GRUB"); TMO=${TMO:-0}
    HID=$(grep -q '^hiddenmenu' "$GRUB" 2>/dev/null && echo 1 || echo 0)
    CRLF=$(grep -lq "$(printf '\r')" "$GRUB" 2>/dev/null && echo 1 || echo 0)

    [ "$NENT" -eq 0 ] 2>/dev/null && fatal "grub.conf has no menu entries (repairable: rebuild)"
    [ "$CRLF" = 1 ] && warn "grub.conf has DOS/CRLF line endings -> GRUB-legacy may misparse (repairable: normalise)"
    [ "$DEF" -ge "$NENT" ] 2>/dev/null && [ "$NENT" -gt 0 ] && warn "default=$DEF is out of range ($NENT entries) -> GRUB may fail (repairable: clamp)"
    grep -nE 'kernel .*init=[^ ]+ .*init=' "$GRUB" >/dev/null 2>&1 && warn "a kernel line has TWO init= parameters (repairable: collapse)"
    if grep -q 'init=/bin/init' "$GRUB" 2>/dev/null && [ "$ROOT_OK" = 0 ]; then
        warn "grub boots init=/bin/init but /bin/init or /sbin/busybox is MISSING -> that entry cannot boot (repairable: drop entry -> factory)"
    fi
    if grep -q 'init=/korg/kronos_init' "$GRUB" 2>/dev/null && ! have "$ROOT/korg/kronos_init"; then
        warn "grub references init=/korg/kronos_init but the target is MISSING -> that entry cannot boot (repairable: strip param)"
    fi
    if [ $ROOTED = 1 ] && [ "$ROOT_OK" = 1 ] && ! grep -q 'init=/bin/init' "$GRUB" 2>/dev/null; then
        warn "rootfs is rooted but grub has NO init=/bin/init entry -> boots factory, not root (repairable: rebuild entry)"
    fi
    if [ $SR = 1 ] && [ "$SR_OK" = 1 ] && ! grep -q 'init=/korg/kronos_init' "$GRUB" 2>/dev/null; then
        warn "ScreenRemote installed but grub has no init=/korg/kronos_init hook -> daemon won't start (repairable: restore hook)"
    fi
    if grep -q 'init=/bin/init' "$GRUB" 2>/dev/null && { [ "$TMO" -lt 3 ] 2>/dev/null || [ "$HID" = 1 ]; }; then
        warn "rooted menu is hard to recover (timeout=$TMO, hiddenmenu=$HID) -> no window to pick factory (repairable: timeout>=3)"
    fi
    # kernel file referenced but absent on /boot (can't recreate a kernel here)
    if [ "$KERNEL_OK" = 0 ] && grep -qE '^[[:space:]]*kernel +/bzImage' "$GRUB" 2>/dev/null; then
        fatal "grub loads /bzImage but $BOOT/bzImage is MISSING -> unbootable (NOT repairable here: use the Restore DVD / autofix, or re-run apply_devroot)"
    fi
fi
[ "$PROBLEMS" = 0 ] && ok "no boot-preventing problems detected"

# ---------------------------------------------------------------------------
# Recommendation
# ---------------------------------------------------------------------------
say ""
case $SCEN in
  1) REC="Factory. To root: run apply_devroot.sh." ;;
  2) REC="Rooted ($ROOTKIND). To unroot: run the DevRoot Uninstaller." ;;
  3) REC="Factory + ScreenRemote. To root: run apply_devroot.sh." ;;
  4) REC="Rooted + ScreenRemote." ;;
esac
say "  recommended: $REC"
if [ "$PROBLEMS" -gt 0 ]; then
    [ "$REPAIR" = 1 ] && say "  -> $PROBLEMS issue(s); applying --repair." \
                      || say "  -> $PROBLEMS issue(s). Re-run with --repair to fix what is repairable."
else
    say "  -> boot chain looks healthy."
fi

[ "$REPAIR" = 1 ] || { say "==================================================="; exit 0; }
[ "$PROBLEMS" = 0 ] && { say ""; say "  --repair: nothing to fix."; exit 0; }

# ---------------------------------------------------------------------------
# Repair
# ---------------------------------------------------------------------------
say ""
say "  --repair: recovering the boot chain ..."
mkdir -p "$BOOT/grub" 2>/dev/null || true

# (a) rootfs permission fixes -- cheap, and a common EACCES-at-PID1 cause.
for t in "$ROOT/bin/init" "$ROOT/sbin/busybox" "$ROOT/korg/kronos_init"; do
    have "$t" && ! execu "$t" && { chmod 755 "$t" 2>/dev/null && say "    chmod +x $t"; }
done

# (b) if grub.conf is missing/empty/no-entries, seed a base factory entry so the
#     surgical pass below has something to work with.  Prefer a backup, then any
#     real kernel line we can find, then the template.
TMP="$BOOT/grub/.grub_tmp"
need_seed=0
{ [ ! -f "$GRUB" ] || ! grep -q '^[[:space:]]*title ' "$GRUB" 2>/dev/null; } && need_seed=1
if [ "$need_seed" = 1 ]; then
    SEED=""
    for b in "$BOOT/grub/grub.conf.bootfix.bak" "$BOOT/grub/grub.conf.bak" "$BOOT/grub/menu.lst"; do
        [ -f "$b" ] && grep -q '^[[:space:]]*title ' "$b" 2>/dev/null && { SEED="$b"; break; }
    done
    if [ -n "$SEED" ]; then
        say "    grub.conf unusable -> restoring from $SEED"
        cp "$SEED" "$GRUB"
    else
        say "    grub.conf unusable and no backup -> writing a template factory entry"
        { echo "title Kronos (factory)"; echo "$CANON_ROOT"; echo "$CANON_KERNEL"; } > "$GRUB"
    fi
fi

# Strip CRLF up front (on a working copy) so every downstream test/awk is clean.
tr -d '\r' < "$GRUB" > "$TMP.lf" 2>/dev/null && mv "$TMP.lf" "$GRUB"

# Filesystem-derived decisions for the surgical awk pass.
STRIP_KI=0; have "$ROOT/korg/kronos_init" || STRIP_KI=1     # target gone -> remove refs
DROP_BI=0;  [ "$ROOT_OK" = 1 ] || DROP_BI=1                 # busybox gone -> drop rooted entry
ADD_SR=0;   { [ "$SR_OK" = 1 ] && [ $SR = 1 ]; } && ADD_SR=1  # SR present -> ensure hook on factory entry

# (c) surgical per-entry rewrite: fix/collapse init=, add SR hook to factory
#     entries, drop un-bootable rooted entries.  Emits ONLY the entry blocks.
awk -v strip_ki="$STRIP_KI" -v drop_bi="$DROP_BI" -v add_sr="$ADD_SR" '
    function flush(){ if(intitle && !(drop_bi && buf ~ /init=\/bin\/init/)) printf "%s", buf }
    BEGIN{ intitle=0 }
    /^[[:space:]]*title /{ flush(); buf=$0 ORS; intitle=1; next }
    {
        if(intitle){
            line=$0
            if(line ~ /^[[:space:]]*kernel /){
                if(strip_ki) gsub(/ +init=\/korg\/kronos_init/,"",line)
                if(line ~ /init=\/bin\/init/){          # DevRoot entry: exactly one init=
                    gsub(/ +init=[^ ]+/,"",line); line=line" init=/bin/init"
                } else if(add_sr && line !~ /init=\/korg\/kronos_init/){  # factory entry: restore SR hook
                    line=line" init=/korg/kronos_init"
                }
            }
            buf=buf line ORS; next
        }
        next   # header lines are recomputed below
    }
    END{ flush() }
' "$GRUB" > "$TMP.entries" 2>/dev/null

# (d) if the rootfs is rooted but no init=/bin/init entry survived, REBUILD one by
#     copying a factory entry's root/kernel lines and adding init=/bin/init.
if [ "$ROOT_OK" = 1 ] && [ -f "$ROOT/etc/inittab.busybox" ] \
   && ! grep -q 'init=/bin/init' "$TMP.entries" 2>/dev/null; then
    B_ROOT=$(awk '/^[[:space:]]*root /{print; exit}' "$TMP.entries")
    B_KERN=$(awk '/^[[:space:]]*kernel /{gsub(/ +init=[^ ]+/,""); print; exit}' "$TMP.entries")
    [ -z "$B_ROOT" ] && B_ROOT="$CANON_ROOT"
    [ -z "$B_KERN" ] && B_KERN="$CANON_KERNEL"
    {
        echo "title Kronos DevRoot (SSH + busybox)"
        echo "$B_ROOT"
        echo "$B_KERN init=/bin/init"
    } >> "$TMP.entries"
    say "    rebuilt the missing init=/bin/init (DevRoot) entry"
fi

# (e) recompute header: default -> a BOOTABLE entry (prefer DevRoot when rooted);
#     rooted units get a recoverable menu, factory stays quiet.
NKEEP=$(grep -c '^[[:space:]]*title ' "$TMP.entries")
if [ "$ROOT_OK" = 1 ] && grep -q 'init=/bin/init' "$TMP.entries" 2>/dev/null; then
    DEFIDX=$(awk '/^[[:space:]]*title /{i++} /init=\/bin\/init/{print i-1; exit}' "$TMP.entries")
    WANT_TMO=3; WANT_HID=0
else
    DEFIDX=0; WANT_TMO=0; WANT_HID=1
fi
{ [ -z "$DEFIDX" ] || [ "$DEFIDX" -ge "$NKEEP" ] 2>/dev/null; } && DEFIDX=0

{
    grep -E '^[[:space:]]*#' "$GRUB" 2>/dev/null | head -20
    echo "default=$DEFIDX"
    echo "timeout=$WANT_TMO"
    [ "$WANT_HID" = 1 ] && echo "hiddenmenu"
    cat "$TMP.entries"
} > "$TMP"
rm -f "$TMP.entries" "$TMP.lf"

# (f) sanity gate, then atomic replace.
if grep -qE '^[[:space:]]*kernel .*bzImage' "$TMP" && [ "$NKEEP" -ge 1 ]; then
    mv "$TMP" "$GRUB"; sync 2>/dev/null
    say "    wrote grub.conf: $NKEEP entries, default=$DEFIDX, timeout=$WANT_TMO, hiddenmenu=$WANT_HID"
    say ""
    say "  new grub.conf:"; sed 's/^/    /' "$GRUB"
    [ "$KERNEL_OK" = 0 ] && { say ""; say "  NOTE: $BOOT/bzImage is still missing -- grub is now consistent but the"; say "        kernel must be restored (autofix / Restore DVD) before it will boot."; }
else
    rm -f "$TMP"
    say "  --repair: ABORTED -- rewritten grub failed sanity check; original left untouched." >&2
    exit 1
fi
say "==================================================="
